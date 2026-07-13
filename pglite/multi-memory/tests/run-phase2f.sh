#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=${1:?main PGlite repository root is required}
MM_ROOT="${REPO_ROOT}/postgres-pglite/pglite/multi-memory"
OUT="${MM_ROOT}/.out/phase2f"
SOURCE_OUT="${MM_ROOT}/.out/phase2c/source-build"
INPUT="${SOURCE_OUT}/bin/pglite.wasm"
GLUE="${SOURCE_OUT}/bin/pglite.js"
PGLITE_ENTRY="${REPO_ROOT}/packages/pglite/dist/index.js"
INLINE="${OUT}/sound-specialized.inline.wasm"
INLINE_REPEAT="${OUT}/sound-specialized.inline.repeat.wasm"
CANDIDATE="${OUT}/sound-specialized.wasm"
CANDIDATE_REPEAT="${OUT}/sound-specialized.repeat.wasm"
CLASSIC="${OUT}/matched-classic.wasm"
DEBUG_INLINE="${OUT}/sound-specialized.debug.inline.wasm"
DEBUG="${OUT}/sound-specialized.debug.wasm"
PROFILE_INLINE="${OUT}/sound-specialized.profile.inline.wasm"
PROFILE="${OUT}/sound-specialized.profile.wasm"
REPORT="${OUT}/sound-specialized.report.json"
DEBUG_REPORT="${OUT}/sound-specialized.debug.report.json"
PROFILE_REPORT="${OUT}/sound-specialized.profile.report.json"
HOST_MANIFEST="${MM_ROOT}/host-import-manifest.json"

rm -rf "${OUT}"
mkdir -p "${OUT}"
if [[ ! -f "${INPUT}" || ! -f "${GLUE}" ]]; then
  echo 'Phase 2F requires the canonical Phase 2C source build; run Phase 2C first.' >&2
  exit 1
fi

HASH=$(sha256sum "${INPUT}" | cut -d' ' -f1)
FEATURES=(
  --enable-feature mutable-globals
  --enable-feature sign-ext
  --enable-feature bulk-memory
  --enable-feature bulk-memory-opt
)
SUMMARIES=()
while IFS= read -r name; do
  [[ -z "${name}" || "${name}" == \#* ]] && continue
  SUMMARIES+=(--private-return-export "${name}")
done <"${MM_ROOT}/private-return-exports.txt"

transform() {
  local output=$1
  local report=$2
  shift 2
  pglite-wasm-multi-memory "${INPUT}" \
    --output "${output}" \
    --report "${report}" \
    --input-sha256 "${HASH}" \
    "${FEATURES[@]}" \
    --provenance \
    --inline-private-fast-path \
    --private-identity-export pgl_private_pointer \
    "${SUMMARIES[@]}" \
    "$@"
}

start_ns=$(date +%s%N)
transform "${INLINE}" "${REPORT}"
end_ns=$(date +%s%N)
transform "${INLINE_REPEAT}" "${OUT}/sound-specialized.repeat.report.json"
cmp "${INLINE}" "${INLINE_REPEAT}"
cmp "${REPORT}" "${OUT}/sound-specialized.repeat.report.json"
printf '{"transformMs":%.3f}\n' \
  "$(awk "BEGIN { print (${end_ns} - ${start_ns}) / 1000000 }")" \
  >"${OUT}/transform-metrics.json"

wasm-opt "${INLINE}" -O3 --all-features -o "${CANDIDATE}"
wasm-opt "${INLINE_REPEAT}" -O3 --all-features -o "${CANDIDATE_REPEAT}"
cmp "${CANDIDATE}" "${CANDIDATE_REPEAT}"

pglite-wasm-multi-memory "${INPUT}" \
  --output "${CLASSIC}" \
  --report "${OUT}/matched-classic.report.json" \
  --input-sha256 "${HASH}" \
  "${FEATURES[@]}" \
  --strip-private-identities-only \
  --private-identity-export pgl_private_pointer

transform "${DEBUG_INLINE}" "${DEBUG_REPORT}" \
  --debug-provenance-assertions
wasm-opt "${DEBUG_INLINE}" -O3 --all-features -o "${DEBUG}"

transform "${PROFILE_INLINE}" "${PROFILE_REPORT}" \
  --profile-memory-accesses
wasm-opt "${PROFILE_INLINE}" -O3 --all-features -o "${PROFILE}"

node22 "${MM_ROOT}/tests/phase1-differential.mjs" \
  "${CLASSIC}" "${CANDIDATE}" "${PGLITE_ENTRY}" \
  "${OUT}/differential.json"
node22 "${MM_ROOT}/tests/phase1-differential.mjs" \
  "${CLASSIC}" "${DEBUG}" "${PGLITE_ENTRY}" \
  "${OUT}/debug-differential.json"

PERFORMANCE=()
for run in 1 2 3; do
  output="${OUT}/performance-${run}.json"
  node22 "${MM_ROOT}/tests/phase1-performance.mjs" \
    "${CLASSIC}" "${CANDIDATE}" "${PGLITE_ENTRY}" "${output}" 1.35
  PERFORMANCE+=("${output}")
done

node22 "${MM_ROOT}/tests/host-import-manifest.mjs" \
  "${CANDIDATE}" "${GLUE}" "${HOST_MANIFEST}" --check \
  >"${OUT}/host-import-summary.json"
node22 "${MM_ROOT}/tests/phase2f-memory-profile.mjs" \
  "${PROFILE}" "${PGLITE_ENTRY}" "${PROFILE_REPORT}" \
  "${OUT}/dynamic-memory-profile.json"

cp "${CANDIDATE}" "${REPO_ROOT}/packages/pglite/dist/pglite.wasm"
PGLITE_PACKAGE="${REPO_ROOT}/packages/pglite"
run_package_tests() {
  cd "${PGLITE_PACKAGE}"
  pnpm test:clean || return
  pnpm exec vitest --minWorkers 1 --maxWorkers 2 \
    --testTimeout 120000 --hookTimeout 120000 \
    tests/*.test.js tests/*.test.ts tests/**/*.test.js tests/**/*.test.ts || return
  pnpm test:clean || return
  pnpm exec vitest --minWorkers 1 --maxWorkers 2 \
    --testTimeout 120000 --hookTimeout 120000 \
    tests/targets/runtimes/node-*.test.js || return
}

if run_package_tests; then
  printf '{"status":"pass","suite":"pglite-basic-and-node"}\n' \
    >"${OUT}/package-tests.json"
else
  printf '{"status":"fail","suite":"pglite-basic-and-node"}\n' \
    >"${OUT}/package-tests.json"
  exit 1
fi

node22 "${MM_ROOT}/tests/phase2f-summary.mjs" \
  "${REPORT}" "${CLASSIC}" "${CANDIDATE}" \
  "${OUT}/differential.json" "${OUT}/debug-differential.json" \
  "${OUT}/dynamic-memory-profile.json" "${OUT}/host-import-summary.json" \
  "${OUT}/package-tests.json" "${OUT}/transform-metrics.json" \
  "${PERFORMANCE[@]}" "${OUT}/summary.json"

echo 'PGlite multi-memory Phase 2F exit gate: PASS'
