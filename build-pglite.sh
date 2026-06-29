#!/bin/bash

### NOTES ###
# $INSTALL_PREFIX is expected to point to the installation folder of various libraries built to wasm (see pglite-builder)
#############

emcc --clear-cache

# final output folder
INSTALL_FOLDER=${INSTALL_FOLDER:-"/pglite"}
PGLITE_BUILD_SHARED_MEMORY=${PGLITE_BUILD_SHARED_MEMORY:-false}
PGLITE_SHARED_MEMORY_SIZE=${PGLITE_SHARED_MEMORY_SIZE:-256MB}
PGLITE_FORCE_CLEAN=${PGLITE_FORCE_CLEAN:-false}
PGLITE_MAKE_JOBS=${PGLITE_MAKE_JOBS:-}

if [ -z "$PGLITE_MAKE_JOBS" ] && [ "$PGLITE_BUILD_SHARED_MEMORY" = true ]; then
    PGLITE_MAKE_JOBS=2
fi

if [ -n "$PGLITE_MAKE_JOBS" ]; then
    PGLITE_MAKE_JOBS_FLAG="-j$PGLITE_MAKE_JOBS"
else
    PGLITE_MAKE_JOBS_FLAG="-j"
fi

if [ "$PGLITE_BUILD_SHARED_MEMORY" = true ]; then
    echo "pglite: building Node shared-memory pglite variant with heap size ${PGLITE_SHARED_MEMORY_SIZE}."
    echo "pglite: shared-memory variant disables prebuilt optional dependency archives and bundled extensions for the core demo build."
    PGLITE_ENVIRONMENT="node,worker"
    PGLITE_USE_PTHREADS=1
    PGLITE_THREAD_CFLAGS="-pthread -matomics -mbulk-memory"
    PGLITE_THREAD_LDFLAGS="-pthread"
    PGLITE_CONFIGURE_ICU="--without-icu"
    PGLITE_CONFIGURE_UUID=""
    PGLITE_CONFIGURE_ZLIB="--without-zlib"
    PGLITE_CONFIGURE_LIBXML="--without-libxml"
    PGLITE_CONFIGURE_LIBXSLT="--without-libxslt"
    PGLITE_ICU_CFLAGS=""
    PGLITE_ICU_LIBS=""
else
    PGLITE_ENVIRONMENT="node,web,worker"
    PGLITE_USE_PTHREADS=0
    PGLITE_THREAD_CFLAGS=""
    PGLITE_THREAD_LDFLAGS=""
    PGLITE_CONFIGURE_ICU="--with-icu"
    PGLITE_CONFIGURE_UUID="--with-uuid=ossp"
    PGLITE_CONFIGURE_ZLIB="--with-zlib"
    PGLITE_CONFIGURE_LIBXML="--with-libxml"
    PGLITE_CONFIGURE_LIBXSLT="--with-libxslt"
    PGLITE_ICU_CFLAGS="-I/install/libs/include"
    PGLITE_ICU_LIBS="-L/install/libs/lib -licui18n -licuuc -licudata"
fi

# build with optimizations by default aka release
PGLITE_CFLAGS="-m32 -sWASM_BIGINT -fpic -sENVIRONMENT=$PGLITE_ENVIRONMENT -sSUPPORT_LONGJMP=emscripten -Wno-declaration-after-statement -Wno-macro-redefined -Wno-unused-function -Wno-missing-prototypes -Wno-incompatible-pointer-types $PGLITE_THREAD_CFLAGS"
if [ "$DEBUG" = true ]
then
    echo "pglite: building debug version."
    PGLITE_CFLAGS="$PGLITE_CFLAGS -g -gsource-map --no-wasm-opt"
else
    echo "pglite: building release version."
    PGLITE_CFLAGS="$PGLITE_CFLAGS -O2"
    # we shouldn't need to do this, but there's a bug somewhere that prevents a successful build if this is set
    unset DEBUG
fi

# PGLITE_OTHER_FLAGS="-sUSE_PTHREADS=0 -fPIC -m32 -mno-bulk-memory -mnontrapping-fptoint -mno-reference-types -mno-sign-ext -mno-extended-const -mno-atomics -mno-tail-call -mno-multivalue -mno-relaxed-simd -mno-simd128 -mno-multimemory -mno-exception-handling -Wno-unused-command-line-argument -Wno-unreachable-code-fallthrough -Wno-unused-function -Wno-invalid-noreturn -Wno-declaration-after-statement -Wno-invalid-noreturn"
# PGLITE_CFLAGS="$PGLITE_CFLAGS"

# first build pglite-libc object WITHOUT the overriding flags
# pushd pglite/src/pglitec && emcc -g --no-wasm-opt -gsource-map -static -fPIC -o pglitec.o -c pglitec.c && popd
pushd pglite/src/pglitec && emcc $PGLITE_CFLAGS -static -fpic -o pglitec.o -c pglitec.c && popd

# -Dread=pgl_read -Dwrite=pgl_write
PGLITE_CFLAGS="$PGLITE_CFLAGS \
-D__PGLITE__ \
-Dsystem=pgl_system -Dpopen=pgl_popen -Dpclose=pgl_pclose \
-Dgeteuid=pgl_geteuid -Dgetuid=pgl_getuid -Dgetpwuid=pgl_getpwuid \
-Dexit=pgl_exit \
-Dmunmap=pgl_munmap \
-Dfcntl=pgl_fcntl \
-Datexit=pgl_atexit \
-Dsetsockopt=pgl_setsockopt -Dgetsockopt=pgl_getsockopt -Dgetsockname=pgl_getsockname \
-Drecv=pgl_recv -Dsend=pgl_send -Dconnect=pgl_connect \
-Dpoll=pgl_poll \
-Dshmget=pgl_shmget -Dshmat=pgl_shmat -Dshmdt=pgl_shmdt -Dshmctl=pgl_shmctl \
-Dlongjmp=pgl_longjmp -Dsiglongjmp=pgl_siglongjmp"
# we don't want to override sigsetjmp and setjmp!
# -Dsigsetjmp=pgl_sigsetjmp -Dsiglongjmp=pgl_siglongjmp \
# -Dsetjmp=pgl_setjmp -Dlongjmp=pgl_longjmp"

echo "pglite: PGLITE_CFLAGS=$PGLITE_CFLAGS"

# run ./configure only if config.status is older than this file
# TODO: we should ALSO check if any of the PGLITE_CFLAGS have changed and trigger a ./configure if they did!!!
REF_FILE="build-pglite.sh"
CONFIG_STATUS="config.status"
BUILD_MODE_STAMP=".pglite-build-mode"
CURRENT_BUILD_MODE="shared-memory:${PGLITE_BUILD_SHARED_MEMORY}"
RUN_CONFIGURE=false
RUN_CLEAN=false

if [ ! -f "$CONFIG_STATUS" ]; then
    echo "$CONFIG_STATUS does not exist, need to run ./configure"
    RUN_CONFIGURE=true
    RUN_CLEAN=true
elif [ "$REF_FILE" -nt "$CONFIG_STATUS" ]; then
    echo "$CONFIG_STATUS is older than $REF_FILE. Need to run ./configure."
    RUN_CONFIGURE=true
    RUN_CLEAN=true
elif [ ! -f "$BUILD_MODE_STAMP" ]; then
    echo "$BUILD_MODE_STAMP does not exist, need to run ./configure"
    RUN_CONFIGURE=true
    RUN_CLEAN=true
elif [ "$(cat "$BUILD_MODE_STAMP")" != "$CURRENT_BUILD_MODE" ]; then
    echo "$BUILD_MODE_STAMP does not match ${CURRENT_BUILD_MODE}. Need to run ./configure."
    RUN_CONFIGURE=true
    RUN_CLEAN=true
else
    echo "$CONFIG_STATUS exists and is newer than $REF_FILE. ./configure will NOT be run."
fi

if [ "$PGLITE_FORCE_CLEAN" = true ]; then
    echo "pglite: forcing clean build because PGLITE_FORCE_CLEAN=true."
    RUN_CLEAN=true
fi

PGLITE_LDFLAGS="-sWASM_BIGINT -sUSE_PTHREADS=$PGLITE_USE_PTHREADS $PGLITE_THREAD_LDFLAGS"
PGLITE_LDFLAGS_SL="-shared -sSIDE_MODULE=1 -Wno-unused-function $PGLITE_THREAD_LDFLAGS"

# we define here "all" emscripten flags in order to allow native builds (like libpglite)
EXPORTED_RUNTIME_METHODS="addFunction,removeFunction,FS,MEMFS,PROXYFS,callMain,ENV,UTF8ToString,stringToNewUTF8,stringToUTF8OnStack"
PGLITE_LDFLAGS_EX="\
-sINITIAL_MEMORY=64MB \
-sWASM_BIGINT \
-sSUPPORT_LONGJMP=emscripten \
-sFORCE_FILESYSTEM=1 \
-sUSE_PTHREADS=$PGLITE_USE_PTHREADS \
$PGLITE_THREAD_LDFLAGS \
-sEXIT_RUNTIME=1 -sENVIRONMENT=$PGLITE_ENVIRONMENT \
-sMAIN_MODULE=2 -sMODULARIZE=1 -sEXPORT_ES6=1 \
-sEXPORT_NAME=Module -sALLOW_TABLE_GROWTH -sALLOW_MEMORY_GROWTH \
-sERROR_ON_UNDEFINED_SYMBOLS=0 \
-sEXPORTED_RUNTIME_METHODS=$EXPORTED_RUNTIME_METHODS \
-sINVOKE_RUN=0 \
-sEXPORTED_FUNCTIONS=_main,_fgets,_fputs,_pclose,_fopen,_fclose,_fflush,___errno_location,_strerror \
$(pwd)/pglite/src/pglitec/pglitec.o \
-lproxyfs.js"

# --with-blocksize=16 
# --disable-largefile 
# --with-blocksize=1 

CONFIGURE_PARAMS="\
ac_cv_exeext=.js \
--host wasm32-unknown-linux-gnu \
--disable-spinlocks \
--without-llvm  \
--without-pam \
--disable-largefile \
--with-openssl=no \
--without-readline \
$PGLITE_CONFIGURE_ICU \
--with-includes=$INSTALL_PREFIX/include:$INSTALL_PREFIX/include/libxml2 \
--with-libraries=$INSTALL_PREFIX/lib \
$PGLITE_CONFIGURE_UUID \
$PGLITE_CONFIGURE_ZLIB \
$PGLITE_CONFIGURE_LIBXML \
$PGLITE_CONFIGURE_LIBXSLT \
--with-template=emscripten \
--prefix=$INSTALL_FOLDER"

# Step 1: configure the project
if [ "$RUN_CONFIGURE" = true ]; then
    LDFLAGS=$PGLITE_LDFLAGS \
    LDFLAGS_SL=$PGLITE_LDFLAGS_SL \
    LDFLAGS_EX=$PGLITE_LDFLAGS_EX \
    ICU_CFLAGS="$PGLITE_ICU_CFLAGS" \
    ICU_LIBS="$PGLITE_ICU_LIBS" \
    CFLAGS=${PGLITE_CFLAGS} emconfigure ./configure $CONFIGURE_PARAMS || { echo 'error: emconfigure failed' ; exit 11; }
    echo "$CURRENT_BUILD_MODE" > "$BUILD_MODE_STAMP"
else
    echo "Warning: configure has not been run because RUN_CONFIGURE=${RUN_CONFIGURE}"
fi

# Step 2: make and install all
if [ "$RUN_CLEAN" = true ]; then
    emmake make PORTNAME=emscripten clean || { echo 'error: emmake make PORTNAME=emscripten clean' ; exit 20; }
fi
emmake make PORTNAME=emscripten $PGLITE_MAKE_JOBS_FLAG || { echo "error: emmake make PORTNAME=emscripten $PGLITE_MAKE_JOBS_FLAG" ; exit 21; }
if [ "$PGLITE_BUILD_SHARED_MEMORY" = true ]; then
    echo "pglite: clearing stale optional extension install artifacts for shared-memory core build."
    rm -rf "$INSTALL_FOLDER/include/postgresql/emscripten/extension/imports" \
           "$INSTALL_FOLDER/lib/postgresql" \
           "$INSTALL_FOLDER/share/postgresql/extension"
fi
emmake make PORTNAME=emscripten install || { echo 'error: emmake make PORTNAME=emscripten install' ; exit 23; }

if [ "$PGLITE_BUILD_SHARED_MEMORY" = true ]; then
    echo "pglite: skipping extension bundle for shared-memory core build."
else
    # Step 3.1: make ported contrib extensions - do not install
    emmake make PORTNAME=emscripten -C contrib/ $PGLITE_MAKE_JOBS_FLAG || { echo "error: emmake make PORTNAME=emscripten -C contrib/ $PGLITE_MAKE_JOBS_FLAG" ; exit 31; }

    # Step 3.2 pgcrypto - special case
    cd ./pglite && ./build-pgcrypto.sh && cd ../

    # Step 3.3: make dist contrib extensions - this will create an archive for each extension
    PGLITE_WITH_PGCRYPTO=1 emmake make PORTNAME=emscripten -C contrib/ dist || { echo 'error: emmake make PORTNAME=emscripten -C contrib/ dist' ; exit 32; }
    # the above will also create a file with the imports that each extension needs - we pass these as input in the next step for emscripten to keep alive

    # Step 4: make and dist other extensions
    SAVE_PATH=$PATH
    PATH=$PATH:$INSTALL_FOLDER/bin
    emmake make OPTFLAGS="" PORTNAME=emscripten -C pglite/other_extensions $PGLITE_MAKE_JOBS_FLAG || { echo "emmake make OPTFLAGS=\"\" PORTNAME=emscripten $PGLITE_MAKE_JOBS_FLAG -C pglite/other_extensions" ; exit 41; }
    # Step 4.1: special case: make PostGIS
    cd ./pglite/ && ./build-postgis.sh && cd ../
    emmake make OPTFLAGS="" PORTNAME=emscripten -C pglite/other_extensions dist || { echo 'emmake make OPTFLAGS="" PORTNAME=emscripten -C pglite/other_extensions dist' ; exit 42; }
    emmake make OPTFLAGS="" PORTNAME=emscripten -C pglite/other_extensions dist-postgis || { echo 'emmake make OPTFLAGS="" PORTNAME=emscripten -C pglite/ dist-postgis' ; exit 43; }
    PATH=$SAVE_PATH
fi

# Step 5: get exported functions
emmake make PORTNAME=emscripten $PGLITE_MAKE_JOBS_FLAG -C src/backend pglite-exported-functions || { echo "emmake make PORTNAME=emscripten $PGLITE_MAKE_JOBS_FLAG -C src/backend pglite-exported-functions" ; exit 51; }

# Step 6: make and install pglite
PGROOT=/pglite
# PG_IMPORTS_DIR=$PGROOT/imports
PGPRELOAD="\
--preload-file $(pwd)/pglite/static/PGPASSFILE@/home/postgres/.pgpass \
--preload-file $(pwd)/pglite/static/empty@/pglite/bin/initdb \
--preload-file $(pwd)/pglite/static/empty@/pglite/bin/pg_dump \
--preload-file $(pwd)/pglite/static/empty@/pglite/bin/postgres \
--preload-file $PGROOT/share/postgresql@/pglite/share/postgresql \
--preload-file $PGROOT/lib/postgresql@/pglite/lib/postgresql \
--preload-file $(pwd)/pglite/static/password@/pglite/password \
--preload-file $(pwd)/pglite/static/empty@/pglite/pgstdin \
--preload-file $(pwd)/pglite/static/empty@/pglite/pgstdout \
--preload-file $(pwd)/pglite/static/locale-a@/pglite/locale-a \
--preload-file $(pwd)/pglite/static/minimal-icu/76.1@/pglite/icu"

PGLITE_EXPORTED_RUNTIME_METHODS="MEMFS,IDBFS,FS,PROXYFS,setValue,getValue,UTF8ToString,stringToNewUTF8,stringToUTF8OnStack,addFunction,removeFunction,callMain,ENV"

# -sDYLINK_DEBUG=2 use this for debugging missing exported symbols (ex when an extension calls a pgcore function that hasn't been exported)
PGLITE_FINAL_MEMORY_FLAGS="-sINITIAL_MEMORY=128MB"
if [ "$PGLITE_BUILD_SHARED_MEMORY" = true ]; then
    PGLITE_FINAL_MEMORY_FLAGS="\
-sINITIAL_MEMORY=$PGLITE_SHARED_MEMORY_SIZE \
-sMAXIMUM_MEMORY=$PGLITE_SHARED_MEMORY_SIZE \
-sSHARED_MEMORY=1 \
-sUSE_PTHREADS=1 \
-sPTHREAD_POOL_SIZE=0 \
-sALLOW_MEMORY_GROWTH=0"
fi

POSTGRES_PGLITE_FLAGS="\
-sSTACK_SIZE=8MB \
$PGLITE_FINAL_MEMORY_FLAGS \
-sIMPORTED_MEMORY=1 \
-sEXPORTED_RUNTIME_METHODS=$PGLITE_EXPORTED_RUNTIME_METHODS \
-sEXPORTED_FUNCTIONS=@/install/pglite/exported_functions.txt \
$PGPRELOAD \
-lnodefs.js -lidbfs.js"

# Building pglite itself needs to be the last step because of the PRELOAD_FILES parameter (a list of files and folders) need to be available.
POSTGRES_PGLITE_FLAGS="$PGLITE_CFLAGS $POSTGRES_PGLITE_FLAGS" emmake make PORTNAME=emscripten -C src/backend/ $PGLITE_MAKE_JOBS_FLAG pglite || { echo "emmake make OPTFLAGS=\"\" PORTNAME=emscripten $PGLITE_MAKE_JOBS_FLAG -C pglite" ; exit 61; }
emmake make PORTNAME=emscripten -C src/backend/ install-pglite || { echo 'emmake make PORTNAME=emscripten -C src/backend/ install-pglite' ; exit 62; }

if [ "$PGLITE_BUILD_SHARED_MEMORY" = true ]; then
    cp "$INSTALL_FOLDER/bin/pglite.js" "$INSTALL_FOLDER/bin/pglite-shared.js" || { echo 'copy pglite-shared.js' ; exit 63; }
    cp "$INSTALL_FOLDER/bin/pglite.wasm" "$INSTALL_FOLDER/bin/pglite-shared.wasm" || { echo 'copy pglite-shared.wasm' ; exit 64; }
    cp "$INSTALL_FOLDER/bin/pglite.data" "$INSTALL_FOLDER/bin/pglite-shared.data" || { echo 'copy pglite-shared.data' ; exit 65; }
fi
