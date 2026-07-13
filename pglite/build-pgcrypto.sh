#!/bin/bash
set -euo pipefail

PGLITE_CFLAGS=${PGLITE_CFLAGS:-}
pushd ../contrib/pgcrypto
# these flags are used in pgxs.mk (postgresql extension makefile) and passed to the build process of that extension
PGLITE_LDFLAGS_SL=${PGLITE_LDFLAGS_SL:-"-sWASM_BIGINT -sSIDE_MODULE=1"}
emmake make LDFLAGS_SL="$PGLITE_LDFLAGS_SL -Wl,--whole-archive -lssl -lcrypto -Wl,--no-whole-archive" CFLAGS_SL="$PGLITE_CFLAGS -sWASM_BIGINT" -j"${PGLITE_BUILD_JOBS:-}"
# emmake make PORTNAME=emscripten dist
popd
