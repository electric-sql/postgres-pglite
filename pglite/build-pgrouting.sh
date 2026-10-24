#!/bin/bash
echo "=== Building pgrouting ==="
set -o pipefail

PGROUTING_SRC=$(pwd)/other_extensions/pgrouting
PGROUTING_BUILD=$PGROUTING_SRC/build
PG_CONFIG=/pglite/bin/pg_config
IMPORTS_DIR=$($PG_CONFIG --pkgincludedir)/emscripten/extension/imports

# the same flags PGXS uses for C extensions (these include the -Dfoo=pgl_foo overrides)
PG_CFLAGS=$($PG_CONFIG --cflags)
# the libc overrides are meant for Postgres C code only, they break C++ standard headers (ie <cstdlib> 'using ::exit')
PG_CXXFLAGS=$(echo "$PG_CFLAGS" | sed -E 's/-D[a-z_]+=pgl_[a-z_]+//g')
# pgRouting translates C++ exceptions into Postgres errors, so exceptions must be catchable (also requires the main module to be linked with exception support)
EXCEPTION_FLAGS="-fexceptions"
# Postgres errors (longjmp) must pass through the C++ try/catch blocks, see emscripten-sjlj-cxx.h
PG_CXXFLAGS="$PG_CXXFLAGS -include $(pwd)/emscripten-sjlj-cxx.h"
# like PostGIS, link libc++ statically: if the main module exported the libc++ symbols pgRouting needs,
# other C++ extensions that bring their own libc++ (ie PostGIS) would resolve part of it to the main module's copy.
# emcc does not link system libraries into a SIDE_MODULE (-lstdc++ is ignored), so pass the PIC archive
# (the variant with Emscripten EH, matching -fexceptions) explicitly.
embuilder --pic build libc++ || { echo 'embuilder libc++ failed' ; exit 45; }
CXX_LIBS="$(em-config CACHE)/sysroot/lib/wasm32-emscripten/pic/libc++.a"
# follow build-pglite.sh: DEBUG=true builds without optimizations
if [ "$DEBUG" = true ]
then
    BUILD_TYPE=Debug
else
    BUILD_TYPE=Release
fi

rm -rf $PGROUTING_BUILD
emcmake cmake -S $PGROUTING_SRC -B $PGROUTING_BUILD \
-DCMAKE_PROJECT_INCLUDE=$(pwd)/pgrouting.emscripten.cmake \
-DCMAKE_BUILD_TYPE=$BUILD_TYPE \
-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
-DPOSTGRESQL_BIN=/pglite/bin \
-DPOSTGRESQL_PG_CONFIG=$PG_CONFIG \
-DBoost_INCLUDE_DIR=$INSTALL_PREFIX/include \
-DBUILD_DOXY=OFF \
-DCMAKE_C_FLAGS="$PG_CFLAGS" \
-DCMAKE_CXX_FLAGS="$PG_CXXFLAGS $EXCEPTION_FLAGS" \
-DCMAKE_SHARED_LINKER_FLAGS="-sSIDE_MODULE=1 -sWASM_BIGINT $EXCEPTION_FLAGS" \
-DCMAKE_CXX_STANDARD_LIBRARIES="$CXX_LIBS" \
|| { echo 'emcmake cmake pgrouting failed' ; exit 46; }

# the Boost Graph heavy C++ sources need a lot of memory each, so don't run an unlimited number of jobs
emmake make -C $PGROUTING_BUILD -j$(nproc) || { echo 'emmake make pgrouting failed' ; exit 47; }

# pgRouting does not use PGXS, so we need to generate the list of symbols it imports from the main module ourselves (see pgxs.mk).
# Read it from the linked .so instead of the .o files, so the statically linked libc++ is not part of it.
mkdir -p $IMPORTS_DIR
node $(pwd)/scripts/side_module_imports.mjs $PGROUTING_BUILD/lib/libpgrouting-*.so > $IMPORTS_DIR/pgrouting.imports || { echo 'generating pgrouting.imports failed' ; exit 48; }
