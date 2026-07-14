#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SHARED_BUILDER_IMAGE=${PGLITE_MULTI_MEMORY_SHARED_BUILDER_IMAGE:-pglite-multi-memory-shared-builder:3.1.74-1}
SHARED_TOOLS_IMAGE=${PGLITE_MULTI_MEMORY_IMAGE:-pglite-multi-memory-shared-tools:3.1.74-1}

PGLITE_MULTI_MEMORY_SHARED_BUILDER_IMAGE="${SHARED_BUILDER_IMAGE}" \
  "${SCRIPT_DIR}/build-shared-builder-image.sh" >/dev/null

PGLITE_MULTI_MEMORY_BUILDER_IMAGE="${SHARED_BUILDER_IMAGE}" \
PGLITE_MULTI_MEMORY_IMAGE="${SHARED_TOOLS_IMAGE}" \
  "${SCRIPT_DIR}/build-tools-image.sh" >/dev/null

printf '%s\n' "${SHARED_TOOLS_IMAGE}"
