#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=${1:?main PGlite repository root is required}
MM_ROOT="${REPO_ROOT}/postgres-pglite/pglite/multi-memory"
PG_ROOT="${REPO_ROOT}/postgres-pglite"
OUT="${MM_ROOT}/.out/phase2a"
CLASSIC="${REPO_ROOT}/packages/pglite/release/pglite.wasm"
PGLITE_ENTRY="${REPO_ROOT}/packages/pglite/dist/index.js"
ORACLE="${OUT}/private-only.wasm"
PROFILE_WASM="${OUT}/function-profile.wasm"
PROFILE_REPORT="${OUT}/function-profile-report.json"
RELEASE_REPORT="${OUT}/release-shapes.report.json"
NAMED_OUT="${OUT}/named-build"
NAMED_WASM="${NAMED_OUT}/bin/pglite.wasm"
NAMED_REPORT="${NAMED_OUT}/pglite.report.json"
PROFILE_DIR="${OUT}/profile"

mkdir -p "${OUT}" "${PROFILE_DIR}"
test -f "${ORACLE}" || {
  echo "Run Phase 2A before its profiling continuation" >&2
  exit 1
}

HASH=$(sha256sum "${CLASSIC}" | cut -d' ' -f1)
FEATURES=(
  --enable-feature mutable-globals
  --enable-feature sign-ext
  --enable-feature bulk-memory
  --enable-feature bulk-memory-opt
)

transform() {
  local input=$1
  local output=$2
  local report=$3
  shift 3
  local hash
  hash=$(sha256sum "${input}" | cut -d' ' -f1)
  pglite-wasm-multi-memory "${input}" \
    --output "${output}" \
    --report "${report}" \
    --input-sha256 "${hash}" \
    "${FEATURES[@]}" \
    "$@"
}

transform "${CLASSIC}" "${OUT}/release-shapes.oracle.wasm" \
  "${RELEASE_REPORT}" --private-only-oracle
transform "${CLASSIC}" "${PROFILE_WASM}" "${PROFILE_REPORT}" \
  --private-only-oracle --profile-function-entries

node22 "${MM_ROOT}/tests/phase2a-function-profile.mjs" \
  "${PROFILE_WASM}" "${PGLITE_ENTRY}" "${PROFILE_REPORT}" \
  "${OUT}/function-entry-counts.json"

rm -f "${PROFILE_DIR}/oracle-100us.cpuprofile"
node22 --cpu-prof --cpu-prof-interval=100 \
  --cpu-prof-dir="${PROFILE_DIR}" \
  --cpu-prof-name=oracle-100us.cpuprofile \
  "${MM_ROOT}/tests/phase1-performance.mjs" --child \
  "${ORACLE}" "${PGLITE_ENTRY}" \
  >"${PROFILE_DIR}/oracle-child-result.json"
node22 "${MM_ROOT}/tests/phase2a-cpu-profile.mjs" \
  "${PROFILE_DIR}/oracle-100us.cpuprofile" "${RELEASE_REPORT}" \
  "${OUT}/cpu-profile-summary.json"

if [[ ! -f "${NAMED_WASM}" || "${PGLITE_PHASE2A_REBUILD_NAMED:-false}" == true ]]; then
  rm -rf "${NAMED_OUT}"
  mkdir -p "${NAMED_OUT}"
  (
    cd "${PG_ROOT}"
    PGLITE_PROFILING_FUNCS=true INSTALL_FOLDER="${NAMED_OUT}" \
      ./build-pglite.sh
  )
fi
transform "${NAMED_WASM}" "${NAMED_OUT}/pglite.oracle.wasm" \
  "${NAMED_REPORT}" --private-only-oracle

for count in 1 8 32 128 256 512; do
  mapfile -t indices < <(
    node22 "${MM_ROOT}/tests/phase2a-select-functions.mjs" \
      "${OUT}/cpu-profile-summary.json" \
      "${OUT}/function-entry-counts.json" "cpu:${count}"
  )
  args=()
  for index in "${indices[@]}"; do
    args+=(--direct-private-function-index "${index}")
  done
  transform "${CLASSIC}" "${OUT}/direct-top-${count}.wasm" \
    "${OUT}/direct-top-${count}-report.json" "${args[@]}"
  node22 "${MM_ROOT}/tests/phase1-performance.mjs" \
    "${CLASSIC}" "${OUT}/direct-top-${count}.wasm" "${PGLITE_ENTRY}" \
    "${OUT}/direct-top-${count}-performance.json" 1.35 || true
done

mapfile -t indices < <(
  node22 "${MM_ROOT}/tests/phase2a-select-functions.mjs" \
    "${OUT}/cpu-profile-summary.json" \
    "${OUT}/function-entry-counts.json" executed
)
args=()
for index in "${indices[@]}"; do
  args+=(--direct-private-function-index "${index}")
done
transform "${CLASSIC}" "${OUT}/direct-executed.wasm" \
  "${OUT}/direct-executed-report.json" "${args[@]}"
node22 "${MM_ROOT}/tests/phase1-performance.mjs" \
  "${CLASSIC}" "${OUT}/direct-executed.wasm" "${PGLITE_ENTRY}" \
  "${OUT}/direct-executed-performance.json" 1.35 || true

diagnostics=()
for count in 1 8 32 128 256 512; do
  diagnostics+=(
    "${OUT}/direct-top-${count}-report.json:${OUT}/direct-top-${count}-performance.json"
  )
done
diagnostics+=(
  "${OUT}/direct-executed-report.json:${OUT}/direct-executed-performance.json"
)
node22 "${MM_ROOT}/tests/phase2a-profile-summary.mjs" \
  "${RELEASE_REPORT}" "${NAMED_REPORT}" \
  "${OUT}/cpu-profile-summary.json" \
  "${OUT}/function-entry-counts.json" \
  "${OUT}/profile-summary.json" "${diagnostics[@]}"
