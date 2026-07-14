#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=${1:?main PGlite repository root is required}
MM_ROOT="${REPO_ROOT}/postgres-pglite/pglite/multi-memory"
POSTMASTER_TEST=/postmaster-test
OUT=/postgres-test
NATIVE="${OUT}/native"
TARGET=${PGLITE_POSTGRES_TEST_TARGET:-check}
JOBS=${PGLITE_POSTGRES_TEST_JOBS:-2}

test -f /.dockerenv || {
  echo 'PostgreSQL regression tests must run inside the pinned Docker image' >&2
  exit 1
}
test "$(uname -m)" = aarch64
[[ "${JOBS}" =~ ^[1-9][0-9]*$ ]] || {
  echo "invalid PostgreSQL test parallel job count: ${JOBS}" >&2
  exit 1
}
export PGLITE_POSTGRES_TEST_JOBS="${JOBS}"
perl -MIPC::Run -e 'print "PostgreSQL TAP dependency: PASS\n"'
test -f "${POSTMASTER_TEST}/artifact/postmaster.wasm"
test -f "${POSTMASTER_TEST}/source-build/bin/pglite.js"
test -f "${POSTMASTER_TEST}/source-build/bin/pglite.data"
test -d "${POSTMASTER_TEST}/icu"

PGLITE_BUILD_JOBS="${JOBS}" \
  "${MM_ROOT}/tests/build-native-regress-tools.sh" "${REPO_ROOT}" "${NATIVE}"
pnpm -C "${REPO_ROOT}/packages/pglite" build >/tmp/pglite-postgres-test-build.log
pnpm -C "${REPO_ROOT}/packages/pglite-socket" build \
  >/tmp/pglite-socket-postgres-test-build.log

PROVIDER=$(node22 "${MM_ROOT}/tools/prepare-test-provider.mjs" \
  "${REPO_ROOT}" "${POSTMASTER_TEST}" "${OUT}" "${NATIVE}")
test "${PROVIDER}" = "${OUT}/provider"
export PGLITE_TEST_PROVIDER="${PROVIDER}"
export PATH="${PROVIDER}/bin:${NATIVE}/build/src/bin/psql:${PATH}"
export LD_LIBRARY_PATH="${NATIVE}/build/src/interfaces/libpq${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export PGCTLTIMEOUT=${PGCTLTIMEOUT:-120}

rm -rf "${OUT}/results/raw-${TARGET}" "${OUT}/results/${TARGET}.log"
mkdir -p "${OUT}/results/raw-${TARGET}"
"${MM_ROOT}/tests/provider-lifecycle.test.sh" \
  "${PROVIDER}" "${OUT}/results/raw-${TARGET}"
set +e
MAKE_OPTIONS=(-C "${NATIVE}/build" -j"${JOBS}")
if [ "${TARGET}" = check-world ]; then
  MAKE_OPTIONS+=(-k)
fi
make "${MAKE_OPTIONS[@]}" "${TARGET}" \
  PGLITE_TEST_CAPABILITY_RUNNER="${PROVIDER}/bin/pglite-test-capability" \
  PROVE="${PROVIDER}/bin/prove" \
  2>&1 | tee "${OUT}/results/${TARGET}.log"
STATUS=${PIPESTATUS[0]}
set -e

node22 "${MM_ROOT}/tools/summarize-postgres-tests.mjs" \
  "${REPO_ROOT}" "${OUT}" "${TARGET}" "${STATUS}"
test "${STATUS}" -eq 0
echo "PGlite PostgreSQL ${TARGET} provider tests: PASS"
