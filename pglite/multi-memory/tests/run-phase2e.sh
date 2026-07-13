#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT=${1:?main PGlite repository root is required}
MM_ROOT="${REPO_ROOT}/postgres-pglite/pglite/multi-memory"
OUT="${MM_ROOT}/.out/phase2e"
SOURCE_OUT="${MM_ROOT}/.out/phase2c/source-build"
GLUE="${SOURCE_OUT}/bin/pglite.js"
CANDIDATE="${MM_ROOT}/.out/phase2c/sound-specialized.wasm"
MANIFEST="${MM_ROOT}/host-import-manifest.json"

mkdir -p "${OUT}"
if [[ ! -f "${CANDIDATE}" || ! -f "${GLUE}" ]]; then
  echo 'Phase 2E requires the exact Phase 2C candidate; run Phase 2C first.' >&2
  exit 1
fi

node22 "${MM_ROOT}/tests/host-import-manifest.mjs" \
  "${CANDIDATE}" "${GLUE}" "${MANIFEST}" --check \
  >"${OUT}/host-import-summary.json"

pnpm --dir="${REPO_ROOT}/packages/pglite" exec vitest run \
  tests/multi-memory-host.test.ts
pnpm --dir="${REPO_ROOT}/packages/pglite" run typecheck
pnpm --dir="${REPO_ROOT}/packages/pglite" exec eslint \
  src/wasm/multi-memory.ts tests/multi-memory-host.test.ts \
  --report-unused-disable-directives --max-warnings 0
pnpm --dir="${REPO_ROOT}/packages/pglite" exec prettier --check \
  src/wasm/multi-memory.ts tests/multi-memory-host.test.ts
pnpm --dir="${REPO_ROOT}" exec prettier --check \
  postgres-pglite/pglite/multi-memory/host-import-policy.mjs \
  postgres-pglite/pglite/multi-memory/tests/host-import-manifest.mjs

echo 'PGlite multi-memory Phase 2E host ABI gate: PASS'
