#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/../../.." && pwd)
IMAGE=${PGLITE_MULTI_MEMORY_SHARED_IMAGE:-pglite-multi-memory-shared-tools:3.1.74-1}
PHASE4_OUT=${PGLITE_MULTI_MEMORY_PHASE4_OUT:-${SCRIPT_DIR}/.out/phase4}
PHASE6_OUT=${PGLITE_MULTI_MEMORY_PHASE6_OUT:-${SCRIPT_DIR}/.out/phase6}

"${SCRIPT_DIR}/build-shared-tools-image.sh" >/dev/null
test "$(docker image inspect "${IMAGE}" --format '{{.Os}}/{{.Architecture}}')" = \
  'linux/arm64'

docker run --rm \
  --env PGLITE_PHASE6_REUSE_ARTIFACT="${PGLITE_PHASE6_REUSE_ARTIFACT:-false}" \
  --env PGLITE_PHASE6_REUSE_SOURCE="${PGLITE_PHASE6_REUSE_SOURCE:-false}" \
  --env PGLITE_PHASE6_DEBUG="${PGLITE_PHASE6_DEBUG:-false}" \
  --env PGLITE_PHASE6_ENABLE_PARALLEL="${PGLITE_PHASE6_ENABLE_PARALLEL:-true}" \
  --env PGLITE_PHASE6_REGRESS_TESTS="${PGLITE_PHASE6_REGRESS_TESTS:-}" \
  --volume "${REPO_ROOT}:/work:rw" \
  --volume "${PHASE4_OUT}:/phase4:rw" \
  --volume "${PHASE6_OUT}:/phase6:rw" \
  --volume pglite-phase5-node-modules:/work/node_modules \
  --volume pglite-multi-memory-pnpm-store:/tmp/pnpm-store \
  --workdir /work \
  "${IMAGE}" \
  bash -lc '
    set -euo pipefail
    export PATH=/opt/node22/bin:${PATH}
    test "$(uname -m)" = aarch64
    pnpm install --frozen-lockfile --store-dir /tmp/pnpm-store
    ./postgres-pglite/pglite/multi-memory/tests/run-phase6.sh /work
  '
