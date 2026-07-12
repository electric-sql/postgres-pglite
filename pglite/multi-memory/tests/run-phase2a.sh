#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=${1:?main PGlite repository root is required}
MM_ROOT="${REPO_ROOT}/postgres-pglite/pglite/multi-memory"
OUT="${MM_ROOT}/.out/phase2a"
CLASSIC="${REPO_ROOT}/packages/pglite/release/pglite.wasm"
PGLITE_ENTRY="${REPO_ROOT}/packages/pglite/dist/index.js"
ORACLE="${OUT}/private-only.wasm"
OUTLINED="${OUT}/outlined-generic.wasm"
INLINE="${OUT}/inline-generic.wasm"

mkdir -p "${OUT}"
test -f "${CLASSIC}"
HASH=$(sha256sum "${CLASSIC}" | cut -d' ' -f1)
FEATURES=(
  --enable-feature mutable-globals
  --enable-feature sign-ext
  --enable-feature bulk-memory
  --enable-feature bulk-memory-opt
)

transform() {
  local output=$1
  local report=$2
  shift 2
  pglite-wasm-multi-memory "${CLASSIC}" \
    --output "${output}" \
    --report "${report}" \
    --input-sha256 "${HASH}" \
    "${FEATURES[@]}" \
    "$@"
}

transform "${ORACLE}" "${OUT}/private-only-report.json" \
  --private-only-oracle
transform "${OUT}/private-only.repeat.wasm" \
  "${OUT}/private-only-report.repeat.json" \
  --private-only-oracle
transform "${OUTLINED}" "${OUT}/outlined-generic-report.json"
transform "${INLINE}" "${OUT}/inline-generic-report.json" \
  --inline-private-fast-path

node22 "${MM_ROOT}/tests/phase2a-audit.mjs" \
  "${CLASSIC}" "${ORACLE}" "${OUT}/private-only.repeat.wasm" \
  "${OUT}/private-only-report.json" "${OUT}/private-only-audit.json"
node22 "${MM_ROOT}/tests/phase1-differential.mjs" \
  "${CLASSIC}" "${ORACLE}" "${PGLITE_ENTRY}" \
  "${OUT}/private-only-differential.json"

for run in 1 2 3; do
  node22 "${MM_ROOT}/tests/phase1-performance.mjs" \
    "${CLASSIC}" "${ORACLE}" "${PGLITE_ENTRY}" \
    "${OUT}/private-only-performance-${run}.json" 1.15 || true
done
node22 "${MM_ROOT}/tests/phase1-performance.mjs" \
  "${CLASSIC}" "${OUTLINED}" "${PGLITE_ENTRY}" \
  "${OUT}/outlined-generic-performance.json" 1.35 || true
node22 "${MM_ROOT}/tests/phase1-performance.mjs" \
  "${CLASSIC}" "${INLINE}" "${PGLITE_ENTRY}" \
  "${OUT}/inline-generic-performance.json" 1.35 || true

node22 "${MM_ROOT}/tests/phase2a-summary.mjs" \
  "${OUT}/private-only-performance-1.json" \
  "${OUT}/private-only-performance-2.json" \
  "${OUT}/private-only-performance-3.json" \
  "${OUT}/outlined-generic-performance.json" \
  "${OUT}/inline-generic-performance.json" \
  "${OUT}/summary.json"
