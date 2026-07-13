#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=${1:?main PGlite repository root is required}
MM_ROOT="${REPO_ROOT}/postgres-pglite/pglite/multi-memory"
OUT="${MM_ROOT}/.out/phase2c"
INPUT="${OUT}/source-build/bin/pglite.wasm"
PGLITE_ENTRY="${REPO_ROOT}/packages/pglite/dist/index.js"
INLINE="${OUT}/sound-specialized.inline.wasm"
INLINE_REPEAT="${OUT}/sound-specialized.inline.repeat.wasm"
CANDIDATE="${OUT}/sound-specialized.wasm"
CANDIDATE_REPEAT="${OUT}/sound-specialized.repeat.wasm"
CLASSIC="${OUT}/matched-classic.wasm"
REPORT="${OUT}/sound-specialized.report.json"
CLASSIC_REPORT="${OUT}/matched-classic.report.json"

mkdir -p "${OUT}"
test -f "${INPUT}"
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
transform "${INLINE_REPEAT}" "${OUT}/sound-specialized.repeat.report.json"
cmp "${INLINE}" "${INLINE_REPEAT}"
wasm-opt "${INLINE}" -O3 --all-features -o "${CANDIDATE}"
wasm-opt "${INLINE_REPEAT}" -O3 --all-features -o "${CANDIDATE_REPEAT}"
cmp "${CANDIDATE}" "${CANDIDATE_REPEAT}"

pglite-wasm-multi-memory "${INPUT}" \
  --output "${CLASSIC}" \
  --report "${CLASSIC_REPORT}" \
  --input-sha256 "${HASH}" \
  "${FEATURES[@]}" \
  --strip-private-identities-only \
  --private-identity-export pgl_private_pointer

node22 "${MM_ROOT}/tests/phase1-differential.mjs" \
  "${CLASSIC}" "${CANDIDATE}" "${PGLITE_ENTRY}" \
  "${OUT}/sound-specialized.differential.json"

PERFORMANCE=()
for run in 1 2 3; do
  output="${OUT}/sound-specialized.performance-${run}.json"
  node22 "${MM_ROOT}/tests/phase1-performance.mjs" \
    "${CLASSIC}" "${CANDIDATE}" "${PGLITE_ENTRY}" "${output}" 1.35
  PERFORMANCE+=("${output}")
done

node22 "${MM_ROOT}/tests/phase2c-summary.mjs" \
  "${REPORT}" "${CLASSIC}" "${CANDIDATE}" \
  "${OUT}/sound-specialized.differential.json" \
  "${PERFORMANCE[@]}" "${OUT}/summary.json"

echo "PGlite multi-memory Phase 2C performance gate: PASS"
