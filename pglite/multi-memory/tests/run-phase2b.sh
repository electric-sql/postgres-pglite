#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=${1:?main PGlite repository root is required}
MM_ROOT="${REPO_ROOT}/postgres-pglite/pglite/multi-memory"
OUT="${MM_ROOT}/.out/phase2b"
CLASSIC="${REPO_ROOT}/packages/pglite/release/pglite.wasm"
PGLITE_ENTRY="${REPO_ROOT}/packages/pglite/dist/index.js"
CANDIDATE="${OUT}/private-roots.wasm"
REPORT="${OUT}/private-roots-report.json"

mkdir -p "${OUT}"
HASH=$(sha256sum "${CLASSIC}" | cut -d' ' -f1)
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

pglite-wasm-multi-memory "${CLASSIC}" \
  --output "${CANDIDATE}" \
  --report "${REPORT}" \
  --input-sha256 "${HASH}" \
  "${FEATURES[@]}" \
  --provenance \
  "${SUMMARIES[@]}"

node22 "${MM_ROOT}/tests/phase1-differential.mjs" \
  "${CLASSIC}" "${CANDIDATE}" "${PGLITE_ENTRY}" \
  "${OUT}/differential.json"
node22 "${MM_ROOT}/tests/phase1-performance.mjs" \
  "${CLASSIC}" "${CANDIDATE}" "${PGLITE_ENTRY}" \
  "${OUT}/performance.json" 1.35 || true
node22 "${MM_ROOT}/tests/phase2b-summary.mjs" \
  "${REPORT}" "${OUT}/performance.json" "${OUT}/summary.json"
