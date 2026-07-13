#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/../../.." && pwd)
IMAGE=${PGLITE_MULTI_MEMORY_IMAGE:-pglite-multi-memory-tools:3.1.74-120}

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
    SOURCE_OUT=/work/postgres-pglite/pglite/multi-memory/.out/phase2c/source-build
    cd /work/postgres-pglite
    PGLITE_MULTI_MEMORY_PROVENANCE=true \
      INSTALL_FOLDER="${SOURCE_OUT}" \
      ./build-pglite.sh
    cd /work
    ./postgres-pglite/pglite/multi-memory/tests/run-phase2c.sh /work
  '
