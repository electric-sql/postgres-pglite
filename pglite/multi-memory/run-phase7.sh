#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/../../.." && pwd)
IMAGE=${PGLITE_MULTI_MEMORY_SHARED_IMAGE:-pglite-multi-memory-shared-tools:3.1.74-1}
PHASE6_OUT=${PGLITE_MULTI_MEMORY_PHASE6_OUT:-${SCRIPT_DIR}/.out/phase6}
PHASE7_OUT=${PGLITE_MULTI_MEMORY_PHASE7_OUT:-${SCRIPT_DIR}/.out/phase7}
PHASE7_NATIVE_VOLUME=${PGLITE_MULTI_MEMORY_PHASE7_NATIVE_VOLUME:-pglite-multi-memory-phase7-native}

"${SCRIPT_DIR}/build-shared-tools-image.sh" >/dev/null
test "$(docker image inspect "${IMAGE}" --format '{{.Os}}/{{.Architecture}}')" = \
  'linux/arm64'

# PostgreSQL's TAP suites exercise chmod(2) failure paths that Docker Desktop's
# macOS bind mounts do not faithfully reproduce. Keep source and durable test
# reports on the host, but build and execute native regression tools on a
# Docker-managed Linux filesystem with the same unprivileged identity used by
# the test process.
docker volume create "${PHASE7_NATIVE_VOLUME}" >/dev/null
docker run --rm \
  --volume "${PHASE7_NATIVE_VOLUME}:/phase7-native" \
  "${IMAGE}" \
  chown 1000:1000 /phase7-native

docker run --rm \
  --user 1000:1000 \
  --env PGLITE_PHASE7_TARGET="${PGLITE_PHASE7_TARGET:-check}" \
  --env PGLITE_PHASE7_JOBS="${PGLITE_PHASE7_JOBS:-2}" \
  --env PGLITE_PROVIDER_DEBUG="${PGLITE_PROVIDER_DEBUG:-false}" \
  --volume "${REPO_ROOT}:/work:rw" \
  --volume "${PHASE6_OUT}:/phase6:rw" \
  --volume "${PHASE7_OUT}:/phase7:rw" \
  --volume "${PHASE7_NATIVE_VOLUME}:/phase7/native:rw" \
  --volume pglite-phase5-node-modules:/work/node_modules \
  --volume pglite-multi-memory-pnpm-store:/tmp/pnpm-store \
  --workdir /work \
  "${IMAGE}" \
  bash -lc '
    set -euo pipefail
    export PATH=/opt/node22/bin:${PATH}
    test "$(uname -m)" = aarch64
    pnpm install --frozen-lockfile --store-dir /tmp/pnpm-store
    ./postgres-pglite/pglite/multi-memory/tests/run-phase7.sh /work
  '
