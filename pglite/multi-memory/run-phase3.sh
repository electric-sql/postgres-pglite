#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/../../.." && pwd)
IMAGE=${PGLITE_MULTI_MEMORY_SHARED_IMAGE:-pglite-multi-memory-shared-tools:3.1.74-1}
PHASE3_OUT=${PGLITE_MULTI_MEMORY_PHASE3_OUT:-${SCRIPT_DIR}/.out/phase3}

"${SCRIPT_DIR}/build-shared-tools-image.sh" >/dev/null

docker run --rm \
  --volume "${REPO_ROOT}:/work:rw" \
  --volume "${PHASE3_OUT}:/phase3:rw" \
  --volume /work/node_modules \
  --volume pglite-multi-memory-pnpm-store:/tmp/pnpm-store \
  --workdir /work \
  "${IMAGE}" \
  bash -lc '
    set -euo pipefail
    export PATH=/opt/node22/bin:${PATH}
    test "${EMCC_CFLAGS}" = "-matomics -mbulk-memory"
    pnpm install --frozen-lockfile --store-dir /tmp/pnpm-store
    pnpm -C packages/pglite build
    cd /work/postgres-pglite/pglite/multi-memory
    ./tests/run.sh
    rm -rf /phase3/*
    mkdir -p /phase3/source-build
    cd /work/postgres-pglite
    # Keep the configure prefix free of "postgres". The PostgreSQL install
    # makefiles use that substring to decide whether to append the normal
    # share/postgresql and lib/postgresql namespaces.
    DEBUG=false \
    PGLITE_SHARED_MEMORY=true \
    PGLITE_MULTI_MEMORY_PROVENANCE=true \
    PGLITE_SKIP_THIRD_PARTY_EXTENSIONS=true \
    PGLITE_BUILD_JOBS=4 \
    INSTALL_FOLDER=/phase3/source-build \
      ./build-pglite.sh
    cd /work
    ./postgres-pglite/pglite/multi-memory/tests/run-phase3.sh /work
  '
