#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/../../.." && pwd)
IMAGE=${PGLITE_MULTI_MEMORY_IMAGE:-pglite-multi-memory-tools:3.1.74-120}

test -f "${REPO_ROOT}/packages/pglite/release/pglite.wasm" || {
  echo "Phase 2A needs packages/pglite/release/pglite.wasm" >&2
  exit 1
}

"${SCRIPT_DIR}/build-tools-image.sh" >/dev/null

docker run --rm \
  --volume "${REPO_ROOT}:/work:rw" \
  --volume /work/node_modules \
  --volume pglite-multi-memory-pnpm-store:/tmp/pnpm-store \
  --workdir /work \
  "${IMAGE}" \
  bash -lc '
    set -euo pipefail
    export PATH=/opt/node22/bin:${PATH}
    pnpm install --frozen-lockfile --store-dir /tmp/pnpm-store
    pnpm -C packages/pglite build
    ./postgres-pglite/pglite/multi-memory/tests/run-phase2a.sh /work
  '
