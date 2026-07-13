#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
IMAGE=${PGLITE_MULTI_MEMORY_IMAGE:-pglite-multi-memory-tools:3.1.74-120}
BUILDER_IMAGE=${PGLITE_MULTI_MEMORY_BUILDER_IMAGE:-electricsql/pglite-builder:3.1.74-7}

docker build \
  --tag "${IMAGE}" \
  --build-arg "PGLITE_BUILDER_IMAGE=${BUILDER_IMAGE}" \
  --file "${SCRIPT_DIR}/Dockerfile" \
  "${SCRIPT_DIR}"

printf '%s\n' "${IMAGE}"
