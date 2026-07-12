#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
IMAGE=${PGLITE_MULTI_MEMORY_IMAGE:-pglite-multi-memory-tools:3.1.74-120}

docker build \
  --tag "${IMAGE}" \
  --file "${SCRIPT_DIR}/Dockerfile" \
  "${SCRIPT_DIR}"

printf '%s\n' "${IMAGE}"
