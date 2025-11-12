#!/bin/bash

### NOTES ###
# $INSTALL_PREFIX is expected to point to the installation folder of various libraries needed by libpglite (see libpglite-builder)
#############

# final output folder
INSTALL_FOLDER=${INSTALL_FOLDER:-"/install/pglite"}

PGLITE_BASE_CFLAGS="-D__PGLITE__"

# build with optimizations by default aka release
PGLITE_CFLAGS="$PGLITE_BASE_CFLAGS -O2"
if [ "$DEBUG" = true ]
then
    echo "pglite: building debug version."
    PGLITE_CFLAGS="$PGLITE_BASE_CFLAGS -g -gsource-map --no-wasm-opt"
else
    echo "pglite: building release version."
    # we shouldn't need to do this, but there's a bug somewhere that prevents a successful build if this is set
    unset DEBUG
fi

echo "pglite: PGLITE_CFLAGS=$PGLITE_CFLAGS"

# run ./configure only if config.status is older than this file
# TODO: we should ALSO check if any of the PGLITE_CFLAGS have changed and trigger a ./configure if they did!!!
REF_FILE="build-libpglite.sh"
CONFIG_STATUS="config.status"
RUN_CONFIGURE=false

if [ ! -f "$CONFIG_STATUS" ]; then
    echo "$CONFIG_STATUS does not exist, need to run ./configure"
    RUN_CONFIGURE=true
elif [ "$REF_FILE" -nt "$CONFIG_STATUS" ]; then
    echo "$CONFIG_STATUS is older than $REF_FILE. Need to run ./configure."
    RUN_CONFIGURE=true
else
    echo "$CONFIG_STATUS exists and is newer than $REF_FILE. ./configure will NOT be run."
fi

# Step 1: configure the project
if [ "$RUN_CONFIGURE" = true ]; then
    CFLAGS="${PGLITE_CFLAGS} -fpic -Wno-declaration-after-statement -Wno-macro-redefined -Wno-unused-function -Wno-missing-prototypes -Wno-incompatible-pointer-types" ./configure --disable-spinlocks --disable-largefile --without-llvm --without-pam --with-openssl=no --without-readline --without-icu --with-includes=$INSTALL_PREFIX/include:$INSTALL_PREFIX/include/libxml2:$(pwd)/pglite/includes --with-libraries=$INSTALL_PREFIX/lib --with-uuid=ossp --with-zlib --with-libxml --with-libxslt --with-template=emscripten --prefix=$INSTALL_FOLDER || { echo 'error: emconfigure failed' ; exit 11; }
else
    echo "Warning: configure has not been run because RUN_CONFIGURE=${RUN_CONFIGURE}"
fi

# Step 2: make and install all except pglite
make PORTNAME=emscripten -j || { echo 'error: make PORTNAME=emscripten -j' ; exit 21; }
make PORTNAME=emscripten install || { echo 'error: make PORTNAME=emscripten install' ; exit 22; }

# Step 3.1: make all contrib extensions - do not install
make PORTNAME=emscripten -C contrib/ -j || { echo 'error: make PORTNAME=emscripten -C contrib/ -j' ; exit 31; }
# Step 3.2: make dist contrib extensions - this will create an archive for each extension
make PORTNAME=emscripten -C contrib/ dist || { echo 'error: make PORTNAME=emscripten -C contrib/ dist' ; exit 32; }
# the above will also create a file with the imports that each extension needs - we pass these as input in the next step for emscripten to keep alive

# Step 4: make and dist other extensions
make OPTFLAGS="" PORTNAME=emscripten -j -C pglite || { echo 'error: make OPTFLAGS="" PORTNAME=emscripten -j -C pglite' ; exit 41; }
make OPTFLAGS="" PORTNAME=emscripten -C pglite/ dist || { echo 'error: make OPTFLAGS="" PORTNAME=emscripten -C pglite/ dist ' ; exit 42; }

# Step 5: make and install libpglite.so
PGROOT=/install/pglite

PGLITE_EMSCRIPTEN_FLAGS="-shared -o pglite.so"
PGLITE_CFLAGS="$PGLITE_CFLAGS $PGLITE_EMSCRIPTEN_FLAGS" make PORTNAME=emscripten -j -C src/backend/ install-pglite || { echo 'make OPTFLAGS="" PORTNAME=emscripten -j -C pglite' ; exit 51; }
