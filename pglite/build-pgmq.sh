#!/bin/bash
pushd other_extensions/pgmq
# these flags are used in pgxs.mk (postgresql extension makefile) and passed to the build process of that extension
emmake make LDFLAGS_SL="-sWASM_BIGINT -sSIDE_MODULE=1" CFLAGS_SL="$PGLITE_CFLAGS -sWASM_BIGINT"
# emmake make PORTNAME=emscripten dist
popd