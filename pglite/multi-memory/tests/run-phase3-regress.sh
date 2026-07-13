#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=${1:?main PGlite repository root is required}
MM_ROOT="${REPO_ROOT}/postgres-pglite/pglite/multi-memory"
OUT="${MM_ROOT}/.out/phase3"
NATIVE="${OUT}/native"
SOURCE_OUT="${OUT}/source-build"
SHARED_PACKAGE="${OUT}/shared-package"
OVERLAY="${OUT}/regress-overlay"
RESULTS="${OUT}/pg-regress"
READY="${OUT}/pg-regress-server.json"
SERVER_LOG="${OUT}/pg-regress-server.log"
PORT=${PGLITE_PHASE3_REGRESS_PORT:-55432}

test -f /.dockerenv || {
  echo 'Phase 3 pg_regress must run inside the pinned Docker image' >&2
  exit 1
}
test -f "${OUT}/sound-specialized.shared.wasm"
test -f "${SOURCE_OUT}/lib/postgresql/regress.so"
test -f "${SOURCE_OUT}/extensions/spi.tar.gz"

"${MM_ROOT}/tests/build-native-regress-tools.sh" "${REPO_ROOT}" "${NATIVE}"
pnpm -C "${REPO_ROOT}/packages/pglite-socket" build

rm -rf "${OVERLAY}" "${RESULTS}" "${OUT}/pg-regress-pgdata"
rm -f "${READY}" "${SERVER_LOG}"
mkdir -p "${OVERLAY}/lib/postgresql" "${RESULTS}"
cp "${SOURCE_OUT}/lib/postgresql/regress.so" \
  "${OVERLAY}/lib/postgresql/regress.so"
tar -xzf "${SOURCE_OUT}/extensions/spi.tar.gz" -C "${OVERLAY}"

SERVER_PID=
cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill -TERM "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

node22 "${MM_ROOT}/tests/phase3-regress-server.mjs" \
  "${REPO_ROOT}/packages/pglite/dist/index.js" \
  "${SHARED_PACKAGE}/dist/index.js" \
  "${REPO_ROOT}/packages/pglite-socket/dist/index.js" \
  "${REPO_ROOT}/packages/pglite/release/pglite.wasm" \
  "${OUT}/sound-specialized.shared.wasm" \
  "${OUT}/pg-regress-pgdata" \
  "${OVERLAY}" \
  "${NATIVE}/source/src/test/regress" \
  "${READY}" \
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
  echo 'Phase 3 regression server did not become ready' >&2
  exit 1
}

export LD_LIBRARY_PATH="${NATIVE}/build/src/interfaces/libpq${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export PGSSLMODE=disable
export PGCONNECT_TIMEOUT=30

REGRESS_TESTS=(
  test_setup boolean char name varchar text int2 int4 int8 oid float4 float8
)

"${NATIVE}/build/src/test/regress/pg_regress" \
  --use-existing \
  --host=127.0.0.1 \
  --port="${PORT}" \
  --user=postgres \
  --dbname=regression \
  --bindir="${NATIVE}/build/src/bin/psql" \
  --inputdir="${NATIVE}/source/src/test/regress" \
  --expecteddir="${NATIVE}/source/src/test/regress" \
  --outputdir="${RESULTS}" \
  --dlpath=/pglite/lib/postgresql \
  --max-connections=1 \
  --max-concurrent-tests=1 \
  "${REGRESS_TESTS[@]}"

node22 - "${RESULTS}/phase3-pg-regress.json" \
  "${NATIVE}/manifest.json" "${READY}" "${REGRESS_TESTS[@]}" <<'NODE'
const fs = require('node:fs')
const [output, nativePath, serverPath, ...tests] = process.argv.slice(2)
fs.writeFileSync(output, `${JSON.stringify({
  schema: 1,
  status: 'pass',
  profile: 'phase3-reduced-concurrency',
  scope: 'single-backend-no-streaming-copy',
  tests,
  testCount: tests.length,
  nativeTools: JSON.parse(fs.readFileSync(nativePath, 'utf8')),
  server: JSON.parse(fs.readFileSync(serverPath, 'utf8')),
}, null, 2)}\n`)
NODE

echo 'PGlite multi-memory Phase 3 selected pg_regress gate: PASS'
