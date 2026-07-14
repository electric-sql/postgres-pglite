#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=${1:?main PGlite repository root is required}
MM_ROOT="${REPO_ROOT}/postgres-pglite/pglite/multi-memory"
OUT=/phase6
PHASE4=/phase4
SOURCE_OUT="${OUT}/source-build"
ARTIFACT_OUT="${OUT}/artifact"
NATIVE="${OUT}/native"
TESTLIB="${OUT}/testlib"
PGDATA="${OUT}/pgdata"
RESULTS="${OUT}/regression"
ISOLATION_RESULTS="${OUT}/isolation"
READY="${OUT}/server-ready.json"
SERVER_RESULT="${OUT}/server-result.json"
SERVER_LOG="${OUT}/server.log"
PORT=${PGLITE_PHASE6_PORT:-55436}

test -f /.dockerenv || {
  echo 'Phase 6 must run inside the pinned Docker image' >&2
  exit 1
}
test "$(uname -m)" = aarch64
test -f "${PHASE4}/source-build/extensions/spi.tar.gz"

if [[ "${PGLITE_PHASE6_REUSE_ARTIFACT:-false}" != true ]]; then
  rm -rf "${ARTIFACT_OUT}"
  if [[ "${PGLITE_PHASE6_REUSE_SOURCE:-false}" != true ]]; then
    rm -rf "${SOURCE_OUT}"
    cp -a "${PHASE4}/source-build" "${SOURCE_OUT}"
  else
    echo 'Reusing the existing Phase 6 configured source build'
    test -f "${SOURCE_OUT}/bin/pglite.js"
  fi
  (
    cd "${REPO_ROOT}/postgres-pglite"
    DEBUG=false \
    PGLITE_INCREMENTAL=true \
    PGLITE_BACKEND_ONLY=true \
    PGLITE_CLEAN_BACKEND=true \
    PGLITE_SHARED_MEMORY=true \
    PGLITE_MULTI_MEMORY_PROVENANCE=true \
    PGLITE_POSTMASTER=true \
    PGLITE_WITH_REGRESSION_TESTS=true \
    PGLITE_SKIP_THIRD_PARTY_EXTENSIONS=true \
    PGLITE_BUILD_JOBS=4 \
    INSTALL_FOLDER="${SOURCE_OUT}" \
      ./build-pglite.sh
  )
  LLVM_NM_BIN=${LLVM_NM:-/emsdk/upstream/bin/llvm-nm}
  TIMESTAMP_SYMBOLS=$(
    "${LLVM_NM_BIN}" \
      "${REPO_ROOT}/postgres-pglite/src/backend/utils/adt/timestamp.o"
  )
  grep -Eq ' U pgl_gettimeofday$' <<<"${TIMESTAMP_SYMBOLS}" || {
    echo 'Phase 6 timestamp object bypasses the PGlite libc clock' >&2
    exit 1
  }
  if grep -Eq ' U gettimeofday$' <<<"${TIMESTAMP_SYMBOLS}"; then
    echo 'Phase 6 timestamp object retained Emscripten gettimeofday' >&2
    exit 1
  fi
  PGLITE_MULTI_MEMORY_PHASE4_INNER_OUT="${ARTIFACT_OUT}" \
  PGLITE_MULTI_MEMORY_PHASE4_SOURCE_OUT="${SOURCE_OUT}" \
    "${MM_ROOT}/tests/run-phase4.sh" "${REPO_ROOT}"
else
  echo 'Reusing the existing Phase 6 regression-enabled artifact'
fi
test -f "${ARTIFACT_OUT}/postmaster.wasm"
test -f "${SOURCE_OUT}/bin/pglite.js"
test -f "${SOURCE_OUT}/bin/pglite.data"
test -f "${SOURCE_OUT}/lib/postgresql/regress.so"

"${MM_ROOT}/tests/build-native-regress-tools.sh" "${REPO_ROOT}" "${NATIVE}"
cc -O2 -Wall -Wextra -Werror \
  -I"${NATIVE}/build/src/include" \
  -I"${NATIVE}/source/src/include" \
  -I"${NATIVE}/source/src/interfaces/libpq" \
  "${MM_ROOT}/tests/phase6-native-client.c" \
  -L"${NATIVE}/build/src/interfaces/libpq" \
  -Wl,-rpath,"${NATIVE}/build/src/interfaces/libpq" \
  -lpq -pthread \
  -o "${OUT}/phase6-native-client"
pnpm -C "${REPO_ROOT}/packages/pglite" build >/tmp/pglite-phase6-build.log
pnpm -C "${REPO_ROOT}/packages/pglite-socket" build \
  >/tmp/pglite-socket-phase6-build.log

rm -rf \
  "${TESTLIB}" "${PGDATA}" "${RESULTS}" "${ISOLATION_RESULTS}" \
  "${OUT}/spi" "${OUT}/icu" \
  "${READY}" "${SERVER_RESULT}" "${SERVER_LOG}"
mkdir -p \
  "${TESTLIB}" "${RESULTS}" "${ISOLATION_RESULTS}" \
  "${OUT}/spi" "${OUT}/icu"
cp "${SOURCE_OUT}/lib/postgresql/regress.so" \
  "${TESTLIB}/regress.so"
tar -xzf "${PHASE4}/source-build/extensions/spi.tar.gz" -C "${OUT}/spi"
tar -xzf "${REPO_ROOT}/packages/pglite-icu-full/static/icu.76.tgz" \
  -C "${OUT}/icu"
cp "${OUT}/spi/lib/postgresql/"*.so "${TESTLIB}/"

node22 "${MM_ROOT}/tests/phase6-correctness.mjs" \
  "${REPO_ROOT}" \
  "${ARTIFACT_OUT}/postmaster.wasm" \
  "${SOURCE_OUT}/bin/pglite.js" \
  "${SOURCE_OUT}/bin/pglite.data" \
  "${OUT}/focused-correctness.json"

SERVER_PID=
cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill -TERM "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

node22 "${MM_ROOT}/tests/phase6-server.mjs" \
  "${REPO_ROOT}" \
  "${ARTIFACT_OUT}/postmaster.wasm" \
  "${SOURCE_OUT}/bin/pglite.js" \
  "${SOURCE_OUT}/bin/pglite.data" \
  "${NATIVE}" \
  "${TESTLIB}" \
  "${OUT}" \
  "${PGDATA}" \
  "${READY}" \
  "${SERVER_RESULT}" \
  "${PORT}" >"${SERVER_LOG}" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 240); do
  [[ -f "${READY}" ]] && break
  if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
    cat "${SERVER_LOG}" >&2
    exit 1
  fi
  sleep 0.25
done
test -f "${READY}" || {
  cat "${SERVER_LOG}" >&2
  echo 'Phase 6 regression server did not become ready' >&2
  exit 1
}

export LD_LIBRARY_PATH="${NATIVE}/build/src/interfaces/libpq${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export PGHOST=127.0.0.1
export PGPORT="${PORT}"
export PGUSER=postgres
export PGSSLMODE=disable
export PGCONNECT_TIMEOUT=30

REGRESS_SELECTION=(
  --schedule="${NATIVE}/source/src/test/regress/parallel_schedule"
)
if [[ -n "${PGLITE_PHASE6_REGRESS_TESTS:-}" ]]; then
  read -r -a REGRESS_SELECTION <<<"${PGLITE_PHASE6_REGRESS_TESTS}"
fi

"${OUT}/phase6-native-client" \
  "host=${PGHOST} port=${PGPORT} user=${PGUSER} dbname=regression sslmode=disable" \
  | tee "${OUT}/native-client.log"

"${NATIVE}/build/src/test/regress/pg_regress" \
  --use-existing \
  --host="${PGHOST}" \
  --port="${PGPORT}" \
  --user="${PGUSER}" \
  --dbname=regression \
  --bindir="${NATIVE}/build/src/bin/psql" \
  --inputdir="${NATIVE}/source/src/test/regress" \
  --expecteddir="${NATIVE}/source/src/test/regress" \
  --outputdir="${RESULTS}" \
  --dlpath="${TESTLIB}" \
  --max-concurrent-tests=20 \
  "${REGRESS_SELECTION[@]}"

if [[ -n "${PGLITE_PHASE6_REGRESS_TESTS:-}" ]]; then
  kill -TERM "${SERVER_PID}"
  wait "${SERVER_PID}"
  SERVER_PID=
  test -f "${SERVER_RESULT}"
  echo "PGlite multi-memory Phase 6 targeted regression gate: PASS (${PGLITE_PHASE6_REGRESS_TESTS})"
  exit 0
fi

"${NATIVE}/build/src/test/isolation/pg_isolation_regress" \
  --use-existing \
  --host="${PGHOST}" \
  --port="${PGPORT}" \
  --user="${PGUSER}" \
  --dbname=isolation_regression \
  --bindir="${NATIVE}/build/src/bin/psql" \
  --inputdir="${NATIVE}/source/src/test/isolation" \
  --expecteddir="${NATIVE}/source/src/test/isolation" \
  --outputdir="${ISOLATION_RESULTS}" \
  --schedule="${NATIVE}/source/src/test/isolation/isolation_schedule"

kill -TERM "${SERVER_PID}"
wait "${SERVER_PID}"
SERVER_PID=
test -f "${SERVER_RESULT}"

node22 - \
  "${NATIVE}/manifest.json" "${READY}" "${SERVER_RESULT}" \
  "${OUT}/core-regression.json" <<'NODE'
const fs = require('node:fs')
const [nativePath, readyPath, serverPath, output] = process.argv.slice(2)
fs.writeFileSync(output, `${JSON.stringify({
  schema: 1,
  status: 'pass',
  profile: 'phase6-full-upstream-concurrency',
  regressionSchedule: 'src/test/regress/parallel_schedule',
  isolationSchedule: 'src/test/isolation/isolation_schedule',
  maxConcurrentTests: 20,
  nativeTools: JSON.parse(fs.readFileSync(nativePath, 'utf8')),
  server: JSON.parse(fs.readFileSync(serverPath, 'utf8')),
}, null, 2)}\n`)
NODE

echo 'PGlite multi-memory Phase 6 core regression and isolation gate: PASS'

node22 "${MM_ROOT}/tests/phase6-stress.mjs" \
  "${REPO_ROOT}" \
  "${ARTIFACT_OUT}/postmaster.wasm" \
  "${SOURCE_OUT}/bin/pglite.js" \
  "${SOURCE_OUT}/bin/pglite.data" \
  "${NATIVE}/build/src/bin/pgbench/pgbench" \
  "${OUT}" \
  "${OUT}/stress.json"
