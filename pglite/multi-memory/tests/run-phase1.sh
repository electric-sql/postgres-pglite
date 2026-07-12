#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=${1:?main PGlite repository root is required}
MM_ROOT="${REPO_ROOT}/postgres-pglite/pglite/multi-memory"
OUT="${MM_ROOT}/.out/phase1"
CLASSIC="${REPO_ROOT}/packages/pglite/release/pglite.wasm"
TRANSFORMED="${OUT}/pglite.multi-memory.wasm"
REPEAT="${OUT}/pglite.multi-memory.repeat.wasm"
REPORT="${OUT}/rewrite-report.json"
PGLITE_ENTRY="${REPO_ROOT}/packages/pglite/dist/index.js"
STATUS=0

mkdir -p "${OUT}"
test -f "${CLASSIC}"

transform() {
  local output=$1
  local report=$2
  pglite-wasm-multi-memory "${CLASSIC}" \
    --output "${output}" \
    --report "${report}" \
    --input-sha256 "$(sha256sum "${CLASSIC}" | cut -d' ' -f1)" \
    --enable-feature mutable-globals \
    --enable-feature sign-ext \
    --enable-feature bulk-memory \
    --enable-feature bulk-memory-opt \
    --inline-private-fast-path
}

start_ns=$(date +%s%N)
transform "${TRANSFORMED}" "${REPORT}"
end_ns=$(date +%s%N)
transform "${REPEAT}" "${OUT}/rewrite-report.repeat.json"
printf '{"transformMs":%.3f}\n' \
  "$(awk "BEGIN { print (${end_ns} - ${start_ns}) / 1000000 }")" \
  >"${OUT}/transform-metrics.json"

node22 "${MM_ROOT}/tests/phase1-audit.mjs" \
  "${CLASSIC}" "${TRANSFORMED}" "${REPEAT}" "${REPORT}" \
  "${OUT}/audit.json"

node22 "${MM_ROOT}/tests/phase1-differential.mjs" \
  "${CLASSIC}" "${TRANSFORMED}" "${PGLITE_ENTRY}" \
  "${OUT}/differential.json"

if ! node22 "${MM_ROOT}/tests/phase1-performance.mjs" \
  "${CLASSIC}" "${TRANSFORMED}" "${PGLITE_ENTRY}" \
  "${OUT}/performance.json"; then
  STATUS=1
fi

# The package build has already copied the classic artifact to dist. Replace
# only that ignored build output so the unmodified existing test suite runs
# against the transformed module without touching the release input.
cp "${TRANSFORMED}" "${REPO_ROOT}/packages/pglite/dist/pglite.wasm"
PGLITE_PACKAGE="${REPO_ROOT}/packages/pglite"
if (
  cd "${PGLITE_PACKAGE}"
  pnpm test:clean
  pnpm exec vitest --minWorkers 1 --maxWorkers 4 \
    --testTimeout 120000 --hookTimeout 120000 \
    tests/*.test.js tests/*.test.ts tests/**/*.test.js tests/**/*.test.ts
  pnpm test:clean
  pnpm exec vitest --minWorkers 1 --maxWorkers 4 \
    --testTimeout 120000 --hookTimeout 120000 \
    tests/targets/runtimes/node-*.test.js
); then
  printf '{"status":"pass","suite":"pglite-basic-and-node"}\n' \
    >"${OUT}/package-tests.json"
else
  printf '{"status":"fail","suite":"pglite-basic-and-node"}\n' \
    >"${OUT}/package-tests.json"
  STATUS=1
fi

if (( STATUS != 0 )); then
  echo "Phase 1 completed with one or more failed gates; see ${OUT}" >&2
  exit "${STATUS}"
fi
echo "Phase 1 gates passed"
