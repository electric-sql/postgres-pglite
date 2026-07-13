#!/usr/bin/env bash
set -euo pipefail

PROVIDER=${1:?provider path is required}
RESULT_ROOT=${2:?result root is required}
PORT=${PGLITE_PHASE7_LIFECYCLE_PORT:-65431}
ROOT=$(mktemp -d "${RESULT_ROOT}/lifecycle.XXXXXX")
PGDATA="${ROOT}/data"
SOCKET_DIR="${ROOT}/socket"
LOG="${ROOT}/postgres.log"

mkdir -p "${SOCKET_DIR}"

cleanup() {
  "${PROVIDER}/bin/pg_ctl" -s -D "${PGDATA}" -m immediate stop \
    >/dev/null 2>&1 || true
}
trap cleanup EXIT

"${PROVIDER}/bin/initdb" -D "${PGDATA}" --auth=trust --no-sync \
  --no-instructions
"${PROVIDER}/bin/pg_ctl" -D "${PGDATA}" -l "${LOG}" \
  -o "-F -k '${SOCKET_DIR}' -p ${PORT}" start
"${PROVIDER}/bin/pg_ctl" -D "${PGDATA}" status
"${PROVIDER}/bin/psql" -X -v ON_ERROR_STOP=1 -h "${SOCKET_DIR}" \
  -p "${PORT}" -d postgres -Atqc 'SELECT 41 + 1'

printf '%s\n' 'log_min_messages = warning' >>"${PGDATA}/postgresql.conf"
"${PROVIDER}/bin/pg_ctl" -D "${PGDATA}" reload

"${PROVIDER}/bin/pg_ctl" -D "${PGDATA}" -l "${LOG}" -t 15 restart
"${PROVIDER}/bin/pg_ctl" -D "${PGDATA}" status
"${PROVIDER}/bin/psql" -X -v ON_ERROR_STOP=1 -h "${SOCKET_DIR}" \
  -p "${PORT}" -d postgres -Atqc \
  "SELECT current_setting('log_min_messages')"

"${PROVIDER}/bin/pg_ctl" -D "${PGDATA}" -m fast stop
set +e
"${PROVIDER}/bin/pg_ctl" -D "${PGDATA}" status >/dev/null 2>&1
STATUS=$?
set -e
test "${STATUS}" -eq 3
test ! -e "${PGDATA}/.pglite-provider.json"
trap - EXIT

echo 'PGlite Phase 7 provider lifecycle: PASS'
