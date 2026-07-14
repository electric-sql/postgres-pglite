#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/../.." && pwd)
IMAGE=${PGLITE_MULTI_MEMORY_IMAGE:-pglite-multi-memory-shared-tools:3.1.74-1}

"${SCRIPT_DIR}/build-image.sh" >/dev/null

docker run --rm \
  --volume "${REPO_ROOT}:/work:rw" \
  --workdir /work/pglite/multi-memory \
  "${IMAGE}" \
  ./tests/run.sh
