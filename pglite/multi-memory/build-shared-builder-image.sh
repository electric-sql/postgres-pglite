#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BUILDER_DIR=$(cd -- "${SCRIPT_DIR}/../builder" && pwd)
IMAGE=${PGLITE_MULTI_MEMORY_SHARED_BUILDER_IMAGE:-pglite-multi-memory-shared-builder:3.1.74-1}

docker build \
  --progress=plain \
  --tag "${IMAGE}" \
  --build-arg 'PGLITE_WASM_FEATURE_FLAGS=-matomics -mbulk-memory' \
  --file "${BUILDER_DIR}/Dockerfile" \
  "${BUILDER_DIR}"

printf '%s\n' "${IMAGE}"
