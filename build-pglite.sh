#!/bin/bash

### NOTES ###
# $INSTALL_PREFIX is expected to point to the installation folder of various libraries built to wasm (see pglite-builder)
#############

emcc --clear-cache

# final output folder
INSTALL_FOLDER=${INSTALL_FOLDER:-"/pglite"}

# build with optimizations by default aka release
PGLITE_CFLAGS="-m32 -sWASM_BIGINT -fpic -sENVIRONMENT=node,web,worker -sSUPPORT_LONGJMP=emscripten -Wno-declaration-after-statement -Wno-macro-redefined -Wno-unused-function -Wno-missing-prototypes -Wno-incompatible-pointer-types"
PGLITE_WASM_FEATURE_FLAGS=""
PGLITE_MEMORY_LDFLAGS="-sUSE_PTHREADS=0"
if [ "${PGLITE_SHARED_MEMORY:-false}" = true ]; then
    echo "pglite: enabling shared Wasm memory without the Emscripten pthread runtime."
    PGLITE_WASM_FEATURE_FLAGS="-matomics -mbulk-memory"
    PGLITE_CFLAGS="$PGLITE_CFLAGS $PGLITE_WASM_FEATURE_FLAGS"
    PGLITE_MEMORY_LDFLAGS="$PGLITE_MEMORY_LDFLAGS -sSHARED_MEMORY=1 -sMAXIMUM_MEMORY=2GB"
    PGLITE_SHARED_MEMORY_RECONFIGURE=true

    # Feature flags are recorded on every object. Never reuse the ordinary
    # single-user object graph for a shared-memory link.
    if [ -f Makefile ]; then
        emmake make clean || { echo 'error: cleaning ordinary Wasm objects for shared build' ; exit 8; }
    fi
fi
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

if [ "${PGLITE_MULTI_MEMORY_PROVENANCE:-false}" = true ]; then
    echo "pglite: enabling explicit multi-memory provenance markers."
    PGLITE_CFLAGS="$PGLITE_CFLAGS -D__PGLITE_MULTI_MEMORY__ -I$(pwd)/pglite/src/pglitec"
    PGLITE_MULTI_MEMORY_RECONFIGURE=true
    # Make does not track command-line flags. Recompile the fenced source that
    # consumes the PGlite libc marker and force the final main-module relink.
    rm -f src/backend/executor/execExprInterp.o
    rm -f src/backend/executor/execTuples.o
    rm -f src/backend/pglite.js src/backend/pglite.wasm src/backend/pglite.data
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
RUN_CONFIGURE=false

if [ "${PGLITE_MULTI_MEMORY_RECONFIGURE:-false}" = true ] ||
   [ "${PGLITE_SHARED_MEMORY_RECONFIGURE:-false}" = true ]; then
    echo "multi-memory/shared build flags require ./configure."
    RUN_CONFIGURE=true
elif [ ! -f "$CONFIG_STATUS" ]; then
    echo "$CONFIG_STATUS does not exist, need to run ./configure"
    RUN_CONFIGURE=true
elif [ "$REF_FILE" -nt "$CONFIG_STATUS" ]; then
    echo "$CONFIG_STATUS is older than $REF_FILE. Need to run ./configure."
    RUN_CONFIGURE=true
else
    echo "$CONFIG_STATUS exists and is newer than $REF_FILE. ./configure will NOT be run."
fi

PGLITE_LDFLAGS="-sWASM_BIGINT $PGLITE_MEMORY_LDFLAGS"
PGLITE_LDFLAGS_SL="-shared -sSIDE_MODULE=1 $PGLITE_WASM_FEATURE_FLAGS $PGLITE_MEMORY_LDFLAGS -Wno-unused-function"

# we define here "all" emscripten flags in order to allow native builds (like libpglite)
EXPORTED_RUNTIME_METHODS="addFunction,removeFunction,FS,MEMFS,PROXYFS,callMain,ENV,UTF8ToString,stringToNewUTF8,stringToUTF8OnStack"
PGLITE_LDFLAGS_EX="\
-sINITIAL_MEMORY=64MB \
-sWASM_BIGINT \
-sSUPPORT_LONGJMP=emscripten \
-sFORCE_FILESYSTEM=1 \
$PGLITE_MEMORY_LDFLAGS \
-sEXIT_RUNTIME=1 -sENVIRONMENT=node,web,worker \
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
--with-icu \
--with-includes=$INSTALL_PREFIX/include:$INSTALL_PREFIX/include/libxml2 \
--with-libraries=$INSTALL_PREFIX/lib \
--with-uuid=ossp \
--with-zlib \
--with-libxml \
--with-libxslt \
--with-template=emscripten \
--prefix=$INSTALL_FOLDER"

# Step 1: configure the project
if [ "$RUN_CONFIGURE" = true ]; then
    LDFLAGS=$PGLITE_LDFLAGS \
    LDFLAGS_SL=$PGLITE_LDFLAGS_SL \
    LDFLAGS_EX=$PGLITE_LDFLAGS_EX \
    ICU_CFLAGS="-I/install/libs/include" \
    ICU_LIBS="-L/install/libs/lib -licui18n -licuuc -licudata" \
    CFLAGS=${PGLITE_CFLAGS} emconfigure ./configure $CONFIGURE_PARAMS || { echo 'error: emconfigure failed' ; exit 11; }
else
    echo "Warning: configure has not been run because RUN_CONFIGURE=${RUN_CONFIGURE}"
fi

# Step 2: make and install all
if [ "${PGLITE_MULTI_MEMORY_PROVENANCE:-false}" = true ]; then
    # The backend's parallel top-level rule can start its link before noticing
    # a manually removed subdirectory object, so rebuild the fenced object
    # explicitly before entering the parallel build. A clean configure has not
    # yet materialized lwlocknames.h and the other generated includes consumed
    # by these objects, so establish that normal top-level prerequisite first.
    emmake make PORTNAME=emscripten -C src/backend generated-headers \
        || { echo 'error: generating headers for provenance-marked executor objects' ; exit 19; }
    emmake make PORTNAME=emscripten -C src/backend/executor \
        execExprInterp.o execTuples.o || { echo 'error: rebuilding provenance-marked executor objects' ; exit 20; }
fi
emmake make PORTNAME=emscripten -j"${PGLITE_BUILD_JOBS:-}" || { echo 'error: emmake make PORTNAME=emscripten -j' ; exit 21; }
emmake make PORTNAME=emscripten install || { echo 'error: emmake make PORTNAME=emscripten install' ; exit 23; }

# Step 3.1: make ported contrib extensions - do not install
emmake make PORTNAME=emscripten -C contrib/ -j"${PGLITE_BUILD_JOBS:-}" || { echo 'error: emmake make PORTNAME=emscripten -C contrib/ -j' ; exit 31; }

# Step 3.2 pgcrypto - special case
cd ./pglite && \
    PGLITE_CFLAGS="${PGLITE_CFLAGS}" \
    PGLITE_LDFLAGS_SL="${PGLITE_LDFLAGS_SL}" \
    PGLITE_BUILD_JOBS="${PGLITE_BUILD_JOBS:-}" \
    ./build-pgcrypto.sh && cd ../

# Step 3.3: make dist contrib extensions - this will create an archive for each extension
PGLITE_WITH_PGCRYPTO=1 emmake make PORTNAME=emscripten -C contrib/ dist \
    ARCHIVE_DIR="$INSTALL_FOLDER/extensions" || { echo 'error: emmake make PORTNAME=emscripten -C contrib/ dist' ; exit 32; }
# the above will also create a file with the imports that each extension needs - we pass these as input in the next step for emscripten to keep alive

# Phase 3's upstream regression gate loads the core regress module directly
# through $libdir. Keep this opt-in so the ordinary PGlite artifact does not
# acquire test-only code, while still using the normal extension-import export
# path for the shared test build.
if [ "${PGLITE_WITH_REGRESSION_TESTS:-false}" = true ]; then
    REGRESS_DIR="src/test/regress"
    REGRESS_IMPORTS_DIR="$INSTALL_FOLDER/include/postgresql/emscripten/extension/imports"
    emmake make PORTNAME=emscripten -C "$REGRESS_DIR" regress.so || \
        { echo 'error: building core regression side module' ; exit 33; }
    mkdir -p "$INSTALL_FOLDER/lib/postgresql" "$REGRESS_IMPORTS_DIR"
    install -m 755 "$REGRESS_DIR/regress.so" \
        "$INSTALL_FOLDER/lib/postgresql/regress.so"
    "$LLVM_NM" --undefined-only "$REGRESS_DIR/regress.o" \
        | awk '{print $2}' \
        | sed '/^$/d' \
        | sort -u > "$REGRESS_DIR/regress.undef.txt"
    "$LLVM_NM" --defined-only "$REGRESS_DIR/regress.o" \
        | awk '$2 ~ /^[TDB]$/ {print $3}' \
        | sed '/^$/d' \
        | sort -u > "$REGRESS_DIR/regress.defs.txt"
    comm -23 "$REGRESS_DIR/regress.undef.txt" "$REGRESS_DIR/regress.defs.txt" \
        > "$REGRESS_IMPORTS_DIR/regress.imports"
fi

# Step 4: make and dist other extensions. Phase 3 deliberately validates the
# complete core dependency/PostgreSQL/contrib world first; dynamic third-party
# side modules remain separately gated.
if [ "${PGLITE_SKIP_THIRD_PARTY_EXTENSIONS:-false}" != true ]; then
    SAVE_PATH=$PATH
    PATH=$PATH:$INSTALL_FOLDER/bin
    emmake make OPTFLAGS="" PORTNAME=emscripten -C pglite/other_extensions -j"${PGLITE_BUILD_JOBS:-}" || { echo 'emmake make OPTFLAGS="" PORTNAME=emscripten -j -C pglite/other_extensions' ; exit 41; }
    # Step 4.1: special case: make PostGIS
    cd ./pglite/ && ./build-postgis.sh && cd ../
    emmake make OPTFLAGS="" PORTNAME=emscripten -C pglite/other_extensions dist || { echo 'emmake make OPTFLAGS="" PORTNAME=emscripten -C pglite/other_extensions dist' ; exit 42; }
    emmake make OPTFLAGS="" PORTNAME=emscripten -C pglite/other_extensions dist-postgis || { echo 'emmake make OPTFLAGS="" PORTNAME=emscripten -C pglite/ dist-postgis' ; exit 43; }
    PATH=$SAVE_PATH
fi

# Step 5: get exported functions
emmake make PORTNAME=emscripten -j"${PGLITE_BUILD_JOBS:-}" -C src/backend pglite-exported-functions || { echo 'emmake make PORTNAME=emscripten -j -C src/backend pglite-exported-functions' ; exit 51; }

# Step 6: make and install pglite
PGROOT=$INSTALL_FOLDER
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
POSTGRES_PGLITE_FLAGS="\
-sSTACK_SIZE=8MB \
-sINITIAL_MEMORY=128MB \
-sIMPORTED_MEMORY=1 \
-sEXPORTED_RUNTIME_METHODS=$PGLITE_EXPORTED_RUNTIME_METHODS \
-sEXPORTED_FUNCTIONS=@/install/pglite/exported_functions.txt \
$PGPRELOAD \
-lnodefs.js -lidbfs.js"

if [ "${PGLITE_PROFILING_FUNCS:-false}" = true ]; then
    echo "pglite: preserving optimized Wasm function names for profiling."
    POSTGRES_PGLITE_FLAGS="$POSTGRES_PGLITE_FLAGS --profiling-funcs"
    # Make does not track command-line flag changes. Force only the final main
    # module to relink; all PostgreSQL and dependency objects remain reusable.
    rm -f src/backend/pglite.js src/backend/pglite.wasm src/backend/pglite.data
fi

# Building pglite itself needs to be the last step because of the PRELOAD_FILES parameter (a list of files and folders) need to be available.
POSTGRES_PGLITE_FLAGS="$PGLITE_CFLAGS $POSTGRES_PGLITE_FLAGS" emmake make PORTNAME=emscripten -C src/backend/ -j"${PGLITE_BUILD_JOBS:-}" pglite || { echo 'emmake make OPTFLAGS="" PORTNAME=emscripten -j -C pglite' ; exit 61; }
emmake make PORTNAME=emscripten -C src/backend/ install-pglite || { echo 'emmake make PORTNAME=emscripten -C src/backend/ install-pglite' ; exit 62; }
