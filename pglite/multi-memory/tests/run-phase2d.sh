#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=${1:?main PGlite repository root is required}
MM_ROOT="${REPO_ROOT}/postgres-pglite/pglite/multi-memory"
OUT="${MM_ROOT}/.out/phase2d"
CLASSIC="${REPO_ROOT}/packages/pglite/release/pglite.wasm"
PGLITE_ENTRY="${REPO_ROOT}/packages/pglite/dist/index.js"
CANDIDATE="${OUT}/guarded-clones.wasm"
REPORT="${OUT}/guarded-clones-report.json"
HASH=$(sha256sum "${CLASSIC}" | cut -d' ' -f1)

mkdir -p "${OUT}"
SUMMARIES=()
while IFS= read -r name; do
  [[ -z "${name}" || "${name}" == \#* ]] && continue
  SUMMARIES+=(--private-return-export "${name}")
done <"${MM_ROOT}/private-return-exports.txt"

CLONES=()
while read -r kind spec _; do
  [[ -z "${kind:-}" || "${kind}" == \#* ]] && continue
  case "${kind}" in
    input-sha256)
      [[ "${spec}" == "${HASH}" ]] || {
        echo "private clone manifest is for ${spec}, input is ${HASH}" >&2
        exit 1
      }
      ;;
    export) CLONES+=(--private-clone-export "${spec}") ;;
    function) CLONES+=(--private-clone-function "${spec}") ;;
    *) echo "unknown private clone manifest entry: ${kind}" >&2; exit 1 ;;
  esac
done <"${MM_ROOT}/private-clones.txt"

pglite-wasm-multi-memory "${CLASSIC}" \
  --output "${CANDIDATE}" \
  --report "${REPORT}" \
  --input-sha256 "${HASH}" \
  --enable-feature mutable-globals \
  --enable-feature sign-ext \
  --enable-feature bulk-memory \
  --enable-feature bulk-memory-opt \
  --provenance "${SUMMARIES[@]}" "${CLONES[@]}"

node22 "${MM_ROOT}/tests/phase1-differential.mjs" \
  "${CLASSIC}" "${CANDIDATE}" "${PGLITE_ENTRY}" \
  "${OUT}/guarded-clones-differential.json"
node22 "${MM_ROOT}/tests/phase1-performance.mjs" \
  "${CLASSIC}" "${CANDIDATE}" "${PGLITE_ENTRY}" \
  "${OUT}/guarded-clones-performance.json" 1.35 || true
node22 "${MM_ROOT}/tests/phase2d-summary.mjs" \
  "${REPORT}" "${OUT}/guarded-clones-performance.json" \
  "${OUT}/summary.json"
