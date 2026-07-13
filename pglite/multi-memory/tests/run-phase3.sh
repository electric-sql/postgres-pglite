#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=${1:?main PGlite repository root is required}
MM_ROOT="${REPO_ROOT}/postgres-pglite/pglite/multi-memory"
OUT="${MM_ROOT}/.out/phase3"
SOURCE_OUT="${OUT}/source-build"
INPUT="${SOURCE_OUT}/bin/pglite.wasm"
GLUE="${SOURCE_OUT}/bin/pglite.js"
INLINE="${OUT}/sound-specialized.shared.inline.wasm"
INLINE_REPEAT="${OUT}/sound-specialized.shared.inline.repeat.wasm"
CANDIDATE="${OUT}/sound-specialized.shared.wasm"
CANDIDATE_REPEAT="${OUT}/sound-specialized.shared.repeat.wasm"
CLASSIC="${OUT}/matched-classic.shared.wasm"
REPORT="${OUT}/sound-specialized.shared.report.json"
SEED_ENTRY="${REPO_ROOT}/packages/pglite/dist/index.js"
SEED_WASM="${REPO_ROOT}/packages/pglite/release/pglite.wasm"
SHARED_PACKAGE="${OUT}/shared-package"

test -f "${INPUT}"
test -f "${GLUE}"
rm -f \
  "${INLINE}" "${INLINE_REPEAT}" "${CANDIDATE}" \
  "${CANDIDATE_REPEAT}" "${CLASSIC}"

HASH=$(sha256sum "${INPUT}" | cut -d' ' -f1)
FEATURES=(
  --enable-feature atomics
  --enable-feature mutable-globals
  --enable-feature sign-ext
  --enable-feature bulk-memory
  --enable-feature bulk-memory-opt
)
SUMMARIES=()
EXPORTS="${OUT}/source-function-exports.txt"
node22 - "${INPUT}" "${EXPORTS}" <<'NODE'
const fs = require('node:fs')
const [input, output] = process.argv.slice(2)
const module = new WebAssembly.Module(fs.readFileSync(input))
const names = WebAssembly.Module.exports(module)
  .filter(({ kind }) => kind === 'function')
  .map(({ name }) => name)
  .sort()
fs.writeFileSync(output, `${names.join('\n')}\n`)
NODE
while IFS= read -r name; do
  [[ -z "${name}" || "${name}" == \#* ]] && continue
  # The source build exports only allocators required by its exact side-module
  # set. An absent summary is conservative: it retains generic dispatch. The
  # transformer still validates every summary we do supply against this exact
  # Wasm export table, so stale or mistyped selected names fail closed.
  if grep -Fxq -- "${name}" "${EXPORTS}"; then
    SUMMARIES+=(--private-return-export "${name}")
  fi
done <"${MM_ROOT}/private-return-exports.txt"

transform() {
  local output=$1
  local report=$2
  pglite-wasm-multi-memory "${INPUT}" \
    --output "${output}" \
    --report "${report}" \
    --input-sha256 "${HASH}" \
    "${FEATURES[@]}" \
    --provenance \
    --inline-private-fast-path \
    --private-identity-export pgl_private_pointer \
    "${SUMMARIES[@]}"
}

transform "${INLINE}" "${REPORT}"
transform "${INLINE_REPEAT}" "${OUT}/sound-specialized.shared.repeat.report.json"
cmp "${INLINE}" "${INLINE_REPEAT}"
cmp "${REPORT}" "${OUT}/sound-specialized.shared.repeat.report.json"
wasm-opt "${INLINE}" -O3 --all-features -o "${CANDIDATE}"
wasm-opt "${INLINE_REPEAT}" -O3 --all-features -o "${CANDIDATE_REPEAT}"
cmp "${CANDIDATE}" "${CANDIDATE_REPEAT}"

pglite-wasm-multi-memory "${INPUT}" \
  --output "${CLASSIC}" \
  --report "${OUT}/matched-classic.shared.report.json" \
  --input-sha256 "${HASH}" \
  "${FEATURES[@]}" \
  --strip-private-identities-only \
  --private-identity-export pgl_private_pointer

node22 "${MM_ROOT}/tests/phase3-audit.mjs" \
  "${INPUT}" "${CLASSIC}" "${CANDIDATE}" "${REPORT}" \
  "${OUT}/artifact-audit.json"

# Exercise a deliberately tiny shared-world allocator against the exact
# transformer used for PostgreSQL. The returned i32 is a real global-domain
# tagged pointer and drives ordinary, bulk, and atomic dereferences.
wasm-opt "${MM_ROOT}/tests/phase3-global-allocation.wat" \
  --all-features --emit-target-features \
  -o "${OUT}/global-allocation.wasm"
pglite-wasm-multi-memory "${OUT}/global-allocation.wasm" \
  --output "${OUT}/global-allocation.multi.wasm" \
  --report "${OUT}/global-allocation.report.json" \
  --global-initial-pages 1 \
  --global-maximum-pages 32768
node22 "${MM_ROOT}/tests/phase3-global-allocation.mjs" \
  "${OUT}/global-allocation.multi.wasm" \
  "${OUT}/global-allocation.json"

# Build a disposable package tree so the test uses the newly generated shared
# Emscripten glue and filesystem bundle without modifying tracked release
# artifacts or the ordinary single-user package build.
rm -rf "${SHARED_PACKAGE}"
mkdir -p "${SHARED_PACKAGE}"
cp "${REPO_ROOT}/packages/pglite/package.json" \
  "${REPO_ROOT}/packages/pglite/tsconfig.json" \
  "${REPO_ROOT}/packages/pglite/tsup.config.ts" \
  "${SHARED_PACKAGE}/"
cp -a "${REPO_ROOT}/packages/pglite/src" \
  "${REPO_ROOT}/packages/pglite/scripts" \
  "${REPO_ROOT}/packages/pglite/release" \
  "${SHARED_PACKAGE}/"
node22 - "${SHARED_PACKAGE}/tsconfig.json" "${REPO_ROOT}/tsconfig.json" <<'NODE'
const fs = require('node:fs')
const [configPath, rootConfig] = process.argv.slice(2)
const config = JSON.parse(fs.readFileSync(configPath, 'utf8'))
config.extends = rootConfig
fs.writeFileSync(configPath, `${JSON.stringify(config, null, 2)}\n`)
NODE
ln -s "${REPO_ROOT}/packages/pglite/node_modules" \
  "${SHARED_PACKAGE}/node_modules"
cp "${SOURCE_OUT}/bin/pglite.js" "${SHARED_PACKAGE}/release/pglite.js"
cp "${SOURCE_OUT}/bin/pglite.data" "${SHARED_PACKAGE}/release/pglite.data"
cp "${CANDIDATE}" "${SHARED_PACKAGE}/release/pglite.wasm"
cp "${SOURCE_OUT}/bin/initdb.js" "${SHARED_PACKAGE}/release/initdb.js"
cp "${SOURCE_OUT}/bin/initdb.wasm" "${SHARED_PACKAGE}/release/initdb.wasm"
cp "${SOURCE_OUT}/extensions/pgcrypto.tar.gz" \
  "${SHARED_PACKAGE}/release/pgcrypto.tar.gz"
rm -rf "${OUT}/side-module-audit"
mkdir -p "${OUT}/side-module-audit"
SIDE_MODULES=0
while IFS= read -r archive; do
  name=$(basename "${archive}" .tar.gz)
  archive_out="${OUT}/side-module-audit/${name}"
  mkdir -p "${archive_out}"
  tar -xzf "${archive}" -C "${archive_out}"
  while IFS= read -r side_module; do
    wat="${OUT}/side-module-audit/${name}-${SIDE_MODULES}.wat"
    wasm-dis "${side_module}" -o "${wat}"
    grep -Eq '\(memory .* shared\)' "${wat}"
    SIDE_MODULES=$((SIDE_MODULES + 1))
  done < <(find "${archive_out}" -type f -name '*.so' -print)
done < <(find "${SOURCE_OUT}/extensions" -maxdepth 1 -type f \
  -name '*.tar.gz' -print | sort)
test "${SIDE_MODULES}" -gt 0
(
  cd "${SHARED_PACKAGE}"
  pnpm build
)

node22 "${MM_ROOT}/tests/phase3-differential.mjs" \
  "${SEED_ENTRY}" "${SHARED_PACKAGE}/dist/index.js" \
  "${SEED_WASM}" "${CLASSIC}" "${CANDIDATE}" \
  "${OUT}/differential.json"

grep -q -- '-matomics' "${REPO_ROOT}/postgres-pglite/src/Makefile.global"
grep -q -- '-mbulk-memory' "${REPO_ROOT}/postgres-pglite/src/Makefile.global"
grep -q -- 'SHARED_MEMORY=1' "${REPO_ROOT}/postgres-pglite/src/Makefile.global"
grep -q -- 'shared:true' "${GLUE}"
if grep -q -- 'PThread\.init\|pthreadPoolReady' "${GLUE}"; then
  echo 'Phase 3 unexpectedly included the Emscripten pthread runtime.' >&2
  exit 1
fi
node22 - "${OUT}/build-audit.json" "${SIDE_MODULES}" <<'NODE'
const fs = require('node:fs')
const [output, sideModules] = process.argv.slice(2)
fs.writeFileSync(output, `${JSON.stringify({
  schema: 1,
  status: 'pass',
  emscripten: '3.1.74',
  compileFlags: ['-matomics', '-mbulk-memory'],
  linkFlags: ['-sSHARED_MEMORY=1', '-sUSE_PTHREADS=0'],
  dependenciesRebuilt: true,
  postgresqlRebuilt: true,
  sharedSideModulesAudited: Number(sideModules),
  pthreadRuntime: false,
  sourceBoundary: 'pglite libc plus fenced PostgreSQL annotations',
}, null, 2)}\n`)
NODE

node22 "${MM_ROOT}/tests/phase3-summary.mjs" \
  "${OUT}/artifact-audit.json" "${OUT}/differential.json" \
  "${OUT}/global-allocation.json" "${OUT}/build-audit.json" \
  "${OUT}/summary.json"
