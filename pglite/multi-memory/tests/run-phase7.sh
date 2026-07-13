#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=${1:?main PGlite repository root is required}
MM_ROOT="${REPO_ROOT}/postgres-pglite/pglite/multi-memory"
PHASE6=/phase6
OUT=/phase7
NATIVE="${OUT}/native"
TARGET=${PGLITE_PHASE7_TARGET:-check}
JOBS=${PGLITE_PHASE7_JOBS:-4}

test -f /.dockerenv || {
  echo 'Phase 7 must run inside the pinned Docker image' >&2
  exit 1
}
test "$(uname -m)" = aarch64
perl -MIPC::Run -e 'print "Phase 7 TAP dependency: PASS\n"'
test -f "${PHASE6}/artifact/postmaster.wasm"
test -f "${PHASE6}/source-build/bin/pglite.js"
test -f "${PHASE6}/source-build/bin/pglite.data"
test -d "${PHASE6}/icu"

PGLITE_BUILD_JOBS="${JOBS}" \
  "${MM_ROOT}/tests/build-native-regress-tools.sh" "${REPO_ROOT}" "${NATIVE}"
pnpm -C "${REPO_ROOT}/packages/pglite" build >/tmp/pglite-phase7-build.log
pnpm -C "${REPO_ROOT}/packages/pglite-socket" build \
  >/tmp/pglite-socket-phase7-build.log

PROVIDER=$(node22 "${MM_ROOT}/tools/prepare-phase7-provider.mjs" \
  "${REPO_ROOT}" "${PHASE6}" "${OUT}" "${NATIVE}")
test "${PROVIDER}" = "${OUT}/provider"
export PGLITE_TEST_PROVIDER="${PROVIDER}"
export PATH="${PROVIDER}/bin:${NATIVE}/build/src/bin/psql:${PATH}"
export LD_LIBRARY_PATH="${NATIVE}/build/src/interfaces/libpq${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export PGCTLTIMEOUT=${PGCTLTIMEOUT:-120}

rm -rf "${OUT}/results/raw-${TARGET}" "${OUT}/results/${TARGET}.log"
mkdir -p "${OUT}/results/raw-${TARGET}"
set +e
make -C "${NATIVE}/build" -j"${JOBS}" "${TARGET}" \
  2>&1 | tee "${OUT}/results/${TARGET}.log"
STATUS=${PIPESTATUS[0]}
set -e

node22 "${MM_ROOT}/tools/summarize-phase7.mjs" \
  "${REPO_ROOT}" "${OUT}" "${TARGET}" "${STATUS}"
test "${STATUS}" -eq 0
echo "PGlite multi-memory Phase 7 ${TARGET} provider gate: PASS"
