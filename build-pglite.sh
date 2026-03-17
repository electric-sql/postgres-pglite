#!/bin/bash

### NOTES ###
# $INSTALL_PREFIX is expected to point to the installation folder of various libraries built to wasm (see pglite-builder)
#############

emcc --clear-cache

# final output folder
INSTALL_FOLDER=${INSTALL_FOLDER:-"/pglite"}

# build with optimizations by default aka release
PGLITE_CFLAGS="-m32 -sWASM_BIGINT -fpic -sENVIRONMENT=node,web,worker -sSUPPORT_LONGJMP=emscripten -Wno-declaration-after-statement -Wno-macro-redefined -Wno-unused-function -Wno-missing-prototypes -Wno-incompatible-pointer-types"
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

PGLITE_LDFLAGS="-sWASM_BIGINT -sUSE_PTHREADS=0"
PGLITE_LDFLAGS_SL="-shared -sSIDE_MODULE=1 -Wno-unused-function"

# we define here "all" emscripten flags in order to allow native builds (like libpglite)
EXPORTED_RUNTIME_METHODS="addFunction,removeFunction,FS,MEMFS,PROXYFS,callMain,ENV,UTF8ToString,stringToNewUTF8,stringToUTF8OnStack"
PGLITE_LDFLAGS_EX="\
-sINITIAL_MEMORY=32MB \
-sWASM_BIGINT \
-sSUPPORT_LONGJMP=emscripten \
-sFORCE_FILESYSTEM=1 \
-sUSE_PTHREADS=0 \
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
--without-icu \
--with-includes=$INSTALL_PREFIX/include:$INSTALL_PREFIX/include/libxml2:$(pwd)/pglite/src/include \
--with-libraries=$INSTALL_PREFIX/lib:$(pwd)/pglite/src/pglite-libc \
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
    CFLAGS=${PGLITE_CFLAGS} emconfigure ./configure $CONFIGURE_PARAMS || { echo 'error: emconfigure failed' ; exit 11; }
else
    echo "Warning: configure has not been run because RUN_CONFIGURE=${RUN_CONFIGURE}"
fi

# Step 2: make and install all
emmake make PORTNAME=emscripten -j || { echo 'error: emmake make PORTNAME=emscripten -j' ; exit 21; }
emmake make PORTNAME=emscripten install || { echo 'error: emmake make PORTNAME=emscripten install' ; exit 23; }

# Step 3.1: make ported contrib extensions - do not install
emmake make PORTNAME=emscripten -C contrib/ -j || { echo 'error: emmake make PORTNAME=emscripten -C contrib/ -j' ; exit 31; }
# Step 3.2: make dist contrib extensions - this will create an archive for each extension
emmake make PORTNAME=emscripten -C contrib/ dist || { echo 'error: emmake make PORTNAME=emscripten -C contrib/ dist' ; exit 32; }
# the above will also create a file with the imports that each extension needs - we pass these as input in the next step for emscripten to keep alive

# Step 4: make and dist other extensions
SAVE_PATH=$PATH
PATH=$PATH:$INSTALL_FOLDER/bin
emmake make OPTFLAGS="" PORTNAME=emscripten -C pglite/other_extensions -j || { echo 'emmake make OPTFLAGS="" PORTNAME=emscripten -j -C pglite/other_extensions' ; exit 41; }
emmake make OPTFLAGS="" PORTNAME=emscripten -C pglite/other_extensions dist || { echo 'emmake make OPTFLAGS="" PORTNAME=emscripten -C pglite/other_extensions dist' ; exit 42; }
PATH=$SAVE_PATH

# Step 5: get exported functions
emmake make PORTNAME=emscripten -j -C src/backend pglite-exported-functions || { echo 'emmake make PORTNAME=emscripten -j -C src/backend pglite-exported-functions' ; exit 51; }

# Step 6: make and install pglite
PGROOT=/pglite
# PG_IMPORTS_DIR=$PGROOT/imports
PGPRELOAD="\
--preload-file $(pwd)/pglite/static/PGPASSFILE@/home/web_user/.pgpass \
--preload-file $(pwd)/pglite/static/empty@/pglite/bin/initdb \
--preload-file $(pwd)/pglite/static/empty@/pglite/bin/pg_dump \
--preload-file $(pwd)/pglite/static/empty@/pglite/bin/postgres \
--preload-file $PGROOT/share/postgresql@/pglite/share/postgresql \
--preload-file $PGROOT/lib/postgresql@/pglite/lib/postgresql \
--preload-file $(pwd)/pglite/static/password@/pglite/password \
--preload-file $(pwd)/pglite/static/empty@/pglite/pgstdin \
--preload-file $(pwd)/pglite/static/empty@/pglite/pgstdout \
--preload-file $(pwd)/pglite/static/locale-a@/pglite/locale-a"

PGLITE_EXPORTED_RUNTIME_METHODS="MEMFS,IDBFS,FS,PROXYFS,setValue,getValue,UTF8ToString,stringToNewUTF8,stringToUTF8OnStack,addFunction,removeFunction,callMain,ENV"

# -sDYLINK_DEBUG=2 use this for debugging missing exported symbols (ex when an extension calls a pgcore function that hasn't been exported)
POSTGRES_PGLITE_FLAGS="\
-sSTACK_SIZE=8MB \
-sINITIAL_MEMORY=128MB \
-sEXPORTED_RUNTIME_METHODS=$PGLITE_EXPORTED_RUNTIME_METHODS \
-sEXPORTED_FUNCTIONS=@/install/pglite/exported_functions.txt \
$PGPRELOAD \
-lnodefs.js -lidbfs.js"

# Building pglite itself needs to be the last step because of the PRELOAD_FILES parameter (a list of files and folders) need to be available.
POSTGRES_PGLITE_FLAGS="$PGLITE_CFLAGS $POSTGRES_PGLITE_FLAGS" emmake make PORTNAME=emscripten -C src/backend/ -j pglite || { echo 'emmake make OPTFLAGS="" PORTNAME=emscripten -j -C pglite' ; exit 61; }
emmake make PORTNAME=emscripten -C src/backend/ install-pglite || { echo 'emmake make PORTNAME=emscripten -C src/backend/ install-pglite' ; exit 62; }

# Step 7.1 pgcrypto - special case
cd ./pglite && ./build-pgcrypto.sh && cd ../
PGLITE_WITH_PGCRYPTO=1 emmake make PORTNAME=emscripten -C contrib/ dist