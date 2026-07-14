#!/bin/bash

### NOTES ###
# $INSTALL_PREFIX is expected to point to the installation folder of various libraries built to wasm (see pglite-builder)
#############

if [ "${PGLITE_INCREMENTAL:-false}" != true ]; then
    emcc --clear-cache
else
    echo "pglite: reusing the configured object graph for an incremental build."
fi

# final output folder
INSTALL_FOLDER=${INSTALL_FOLDER:-"/pglite"}

# build with optimizations by default aka release
PGLITE_CFLAGS="-m32 -sWASM_BIGINT -fpic -sENVIRONMENT=node,web,worker -sSUPPORT_LONGJMP=emscripten -Wno-declaration-after-statement -Wno-macro-redefined -Wno-unused-function -Wno-missing-prototypes -Wno-incompatible-pointer-types"
PGLITE_WASM_FEATURE_FLAGS=""
PGLITE_MEMORY_LDFLAGS="-sUSE_PTHREADS=0"
POSTGRES_PGLITE_INITIAL_MEMORY=${PGLITE_PRIVATE_INITIAL_MEMORY:-128MB}
POSTGRES_PGLITE_MAXIMUM_MEMORY=${PGLITE_PRIVATE_MAXIMUM_MEMORY:-2GB}
if [ "${PGLITE_POSTMASTER:-false}" = true ]; then
    POSTGRES_PGLITE_INITIAL_MEMORY=${PGLITE_PRIVATE_INITIAL_MEMORY:-32MB}
    POSTGRES_PGLITE_MAXIMUM_MEMORY=${PGLITE_PRIVATE_MAXIMUM_MEMORY:-1GB}
fi
if [ "${PGLITE_SHARED_MEMORY:-false}" = true ]; then
    echo "pglite: enabling shared Wasm memory without the Emscripten pthread runtime."
    PGLITE_WASM_FEATURE_FLAGS="-matomics -mbulk-memory"
    PGLITE_CFLAGS="$PGLITE_CFLAGS $PGLITE_WASM_FEATURE_FLAGS"
    PGLITE_MEMORY_LDFLAGS="$PGLITE_MEMORY_LDFLAGS -sSHARED_MEMORY=1 -sMAXIMUM_MEMORY=${POSTGRES_PGLITE_MAXIMUM_MEMORY}"
    PGLITE_SHARED_MEMORY_RECONFIGURE=true

    # Feature flags are recorded on every object. Never reuse the ordinary
    # single-user object graph for a shared-memory link.
    if [ -f Makefile ] && [ "${PGLITE_INCREMENTAL:-false}" != true ]; then
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

if [ "${PGLITE_POSTMASTER:-false}" = true ]; then
    echo "pglite: enabling the Worker-backed EXEC_BACKEND portability layer."
    PGLITE_CFLAGS="$PGLITE_CFLAGS -D__PGLITE_POSTMASTER__ \
-I$(pwd)/pglite/src/pglitec -include $(pwd)/pglite/src/pglitec/pglitec.h"
    PGLITE_POSTMASTER_RECONFIGURE=true
    # The forced PGlite libc header redirects the dynamic-loader boundary.
    # Make does not otherwise notice a change to this injected header.
    rm -f src/backend/libpq/auth.o
    rm -f src/backend/utils/fmgr/dfmgr.o
    rm -f src/backend/utils/misc/stack_depth.o
    rm -f src/backend/postmaster/checkpointer.o
fi

# PGLITE_OTHER_FLAGS="-sUSE_PTHREADS=0 -fPIC -m32 -mno-bulk-memory -mnontrapping-fptoint -mno-reference-types -mno-sign-ext -mno-extended-const -mno-atomics -mno-tail-call -mno-multivalue -mno-relaxed-simd -mno-simd128 -mno-multimemory -mno-exception-handling -Wno-unused-command-line-argument -Wno-unreachable-code-fallthrough -Wno-unused-function -Wno-invalid-noreturn -Wno-declaration-after-statement -Wno-invalid-noreturn"
# PGLITE_CFLAGS="$PGLITE_CFLAGS"

# first build pglite-libc object WITHOUT the overriding flags
# pushd pglite/src/pglitec && emcc -g --no-wasm-opt -gsource-map -static -fPIC -o pglitec.o -c pglitec.c && popd
pushd pglite/src/pglitec && emcc $PGLITE_CFLAGS -DPGLITEC_IMPLEMENTATION -static -fpic -o pglitec.o -c pglitec.c && popd

# -Dread=pgl_read -Dwrite=pgl_write
PGLITE_CFLAGS="$PGLITE_CFLAGS \
-D__PGLITE__ \
-Dsystem=pgl_system -Dpopen=pgl_popen -Dpclose=pgl_pclose \
-Dgeteuid=pgl_geteuid -Dgetuid=pgl_getuid \
-Dgetpwuid=pgl_getpwuid -Dgetpwuid_r=pgl_getpwuid_r \
-Dexit=pgl_exit \
-Dmunmap=pgl_munmap \
-Dfcntl=pgl_fcntl \
-Datexit=pgl_atexit \
-Dsetsockopt=pgl_setsockopt -Dgetsockopt=pgl_getsockopt -Dgetsockname=pgl_getsockname \
-Drecv=pgl_recv -Dsend=pgl_send -Dconnect=pgl_connect \
-Dpoll=pgl_poll \
-Dshmget=pgl_shmget -Dshmat=pgl_shmat -Dshmdt=pgl_shmdt -Dshmctl=pgl_shmctl \
-Dlongjmp=pgl_longjmp -Dsiglongjmp=pgl_siglongjmp"
if [ "${PGLITE_POSTMASTER:-false}" = true ]; then
    PGLITE_CFLAGS="$PGLITE_CFLAGS \
-Dgetpid=pgl_getpid -Dkill=pgl_kill -Dwaitpid=pgl_waitpid \
-Dsetitimer=pgl_setitimer -Dsigprocmask=pgl_sigprocmask \
-Dsocket=pgl_socket -Dbind=pgl_bind -Dlisten=pgl_listen \
-Daccept=pgl_accept -Dclose=pgl_close \
-Dread=pgl_fd_read -Dwrite=pgl_fd_write \
-Dpread=pgl_fd_pread -Dpwrite=pgl_fd_pwrite \
-Dreadv=pgl_fd_readv -Dwritev=pgl_fd_writev \
-Dpreadv=pgl_fd_preadv -Dpwritev=pgl_fd_pwritev \
-Dgettimeofday=pgl_gettimeofday"
fi
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
   [ "${PGLITE_SHARED_MEMORY_RECONFIGURE:-false}" = true ] ||
   [ "${PGLITE_POSTMASTER_RECONFIGURE:-false}" = true ]; then
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

# Prepare the final main-module link. The backend-only path uses the already
# installed PostgreSQL data and extension set while rebuilding just the core
# objects and generated module.
PGROOT=$INSTALL_FOLDER
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
POSTGRES_PGLITE_FLAGS="\
-sSTACK_SIZE=8MB \
-sINITIAL_MEMORY=${POSTGRES_PGLITE_INITIAL_MEMORY} \
-sIMPORTED_MEMORY=1 \
-sEXPORTED_RUNTIME_METHODS=$PGLITE_EXPORTED_RUNTIME_METHODS \
-sEXPORTED_FUNCTIONS=@/install/pglite/exported_functions.txt \
$PGPRELOAD \
-lnodefs.js -lidbfs.js"

build_regression_test_modules() {
    REGRESS_DIR="src/test/regress"
    REGRESS_IMPORTS_DIR="$INSTALL_FOLDER/include/postgresql/emscripten/extension/imports"
    STATIC_TEST_DIR="$INSTALL_FOLDER/pglite-static-tests"
    STATIC_MODULE_ARGS=()
    PGLITE_STATIC_TEST_LIBS=""

    static_module_c_name() {
        local name

        name=$(printf '%s' "$1" | sed 's/[^A-Za-z0-9_]/_/g')
        if [[ ! "$name" =~ ^[A-Za-z] ]]; then
            name="m_$name"
        fi
        printf '%s' "$name"
    }

    # Build one loadable module twice.  The first compile discovers its
    # PostgreSQL loader entry points; the second gives those process-local
    # symbols deterministic names so many modules can coexist in one main
    # module.  SQL-visible functions retain their ordinary dlsym names in the
    # generated registry.
    build_static_module() {
        local module_dir=$1
        local module_name=$2
        shift 2
        local module_objects=("$@")
        local c_name
        local base_cppflags
        local cppflags
        local entry
        local resolved
        local entry_points=()

        c_name=$(static_module_c_name "$module_name")
        rm -f "${module_objects[@]}" "$module_dir/$module_name.so"
        emmake make PORTNAME=emscripten -C "$module_dir" \
            "$module_name.so" || {
            echo "error: discovering static module $module_name"
            exit 42
        }
        base_cppflags=$(emmake make -s PORTNAME=emscripten -C "$module_dir" \
            --eval='pglite-print-cppflags: ; @printf "%s" "$(CPPFLAGS)"' \
            pglite-print-cppflags 2>/dev/null | tail -n 1)
        mapfile -t entry_points < <(
            "$LLVM_NM" --defined-only --extern-only "${module_objects[@]}" \
                | awk '$2 ~ /^[Tt]$/ && $3 ~ /^_PG_/ {print $3}' \
                | sort -u
        )

        cppflags="-DPg_magic_func=pgl_static_${c_name}_Pg_magic_func"
        for entry in "${entry_points[@]}"; do
            resolved="pgl_static_${c_name}${entry}"
            cppflags="$cppflags -D${entry}=${resolved}"
        done
        rm -f "${module_objects[@]}" "$module_dir/$module_name.so"
        emmake make PORTNAME=emscripten \
            CPPFLAGS="$base_cppflags $cppflags" \
            -C "$module_dir" "$module_name.so" || {
            echo "error: building namespaced static module $module_name"
            exit 43
        }

        STATIC_MODULE_ARGS+=(
            --module "$module_name" "${module_objects[@]}"
        )
        for entry in "${entry_points[@]}"; do
            resolved="pgl_static_${c_name}${entry}"
            STATIC_MODULE_ARGS+=(
                --alias "$module_name:$entry=$resolved"
            )
        done
    }

    # Discover the exact configured PGXS module set rather than maintaining a
    # second extension list.  Standalone test programs remain native host
    # tools; only server modules and their extension data enter the Wasm
    # artifact.
    build_static_pgxs_tree() {
        local tree=$1
        local install_data=$2
        local subdirs
        local subdir
        local module_dir
        local values
        local module_big
        local modules
        local objects
        local shlib_link
        local extensions
        local data
        local data_built
        local module
        local object
        local module_objects=()

        subdirs=$(emmake make -s PGLITE_WITH_PGCRYPTO=1 \
            PORTNAME=emscripten -C "$tree" \
            --eval='pglite-print-vars: ; @printf "%s" "$(SUBDIRS)"' \
            pglite-print-vars 2>/dev/null | tail -n 1)
        for subdir in $subdirs; do
            module_dir="$tree/$subdir"
            values=$(emmake make -s PGLITE_WITH_PGCRYPTO=1 \
                PORTNAME=emscripten -C "$module_dir" \
                --eval='pglite-print-vars: ; @printf "%s|%s|%s|%s|%s|%s|%s" "$(MODULE_big)" "$(MODULES)" "$(OBJS)" "$(SHLIB_LINK)" "$(EXTENSION)" "$(DATA)" "$(DATA_built)"' \
                pglite-print-vars 2>/dev/null | tail -n 1)
            IFS='|' read -r module_big modules objects shlib_link \
                extensions data data_built <<<"$values"
            if [ -n "$module_big" ]; then
                module_objects=()
                for object in $objects; do
                    module_objects+=("$module_dir/$object")
                done
                build_static_module "$module_dir" "$module_big" \
                    "${module_objects[@]}"
            fi
            for module in $modules; do
                build_static_module "$module_dir" "$module" \
                    "$module_dir/$module.o"
            done
            if [ -n "$shlib_link" ]; then
                PGLITE_STATIC_TEST_LIBS="$PGLITE_STATIC_TEST_LIBS $shlib_link"
            fi
            # pgcrypto is deliberately built even though the PostgreSQL core
            # is configured without OpenSSL.  Its normal PGlite side-module
            # build supplies these archives explicitly; retain the same
            # dependency boundary when folding it into the world-test binary.
            if [ "$module_big" = pgcrypto ]; then
                PGLITE_STATIC_TEST_LIBS="$PGLITE_STATIC_TEST_LIBS -Wl,--whole-archive -lssl -lcrypto -Wl,--no-whole-archive"
            fi
            if [ "$install_data" = true ] && \
               [ -n "$module_big$modules$extensions$data$data_built" ]; then
                emmake make PORTNAME=emscripten PROGRAM= \
                    -C "$module_dir" install || {
                    echo "error: installing static test module data from $module_dir"
                    exit 44
                }
            fi
        done
    }

    rm -f "$REGRESS_DIR/regress.o" "$REGRESS_DIR/regress.so"
    emmake make PORTNAME=emscripten \
        CPPFLAGS="-DPg_magic_func=pgl_static_regress_Pg_magic_func" \
        -C "$REGRESS_DIR" regress.so || \
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
    STATIC_MODULE_ARGS+=(--module regress "$REGRESS_DIR/regress.o")

    # Core regression exercises Datums backed by shared buffers. Ordinary
    # Emscripten side modules only understand memory 0, so the test artifact
    # links the core test module, PL/pgSQL, and encoding conversions into the
    # main module before the multi-memory transform. Installed .so files stay
    # present for PostgreSQL's normal filename/stat checks; the PGlite libc
    # loader resolves these modules from the generated static registry.
    rm -f src/pl/plpgsql/src/*.o src/pl/plpgsql/src/plpgsql.so
    emmake make PORTNAME=emscripten \
        CPPFLAGS="-DPg_magic_func=pgl_static_plpgsql_Pg_magic_func" \
        -C src/pl/plpgsql/src install || \
        { echo 'error: building static PL/pgSQL test module' ; exit 34; }

    STATIC_MODULE_ARGS+=(--module plpgsql src/pl/plpgsql/src/*.o)

    # The earlier single-instance regression artifact needs only test_dsa.
    # Postmaster artifacts below discover and install the complete configured
    # world-test module set, which includes test_dsa.
    if [ "${PGLITE_POSTMASTER:-false}" != true ]; then
        rm -f src/test/modules/test_dsa/test_dsa.o \
            src/test/modules/test_dsa/test_dsa.so
        emmake make PORTNAME=emscripten \
            CPPFLAGS="-DPg_magic_func=pgl_static_test_dsa_Pg_magic_func" \
            -C src/test/modules/test_dsa install || \
            { echo 'error: building static DSA test module' ; exit 38; }
        STATIC_MODULE_ARGS+=(
            --module test_dsa src/test/modules/test_dsa/test_dsa.o
        )
    fi

    rm -f src/backend/replication/libpqwalreceiver/libpqwalreceiver.o \
        src/backend/replication/libpqwalreceiver/libpqwalreceiver.so
    emmake make PORTNAME=emscripten \
        CPPFLAGS="-DPg_magic_func=pgl_static_libpqwalreceiver_Pg_magic_func -D_PG_init=pgl_static_libpqwalreceiver_PG_init" \
        -C src/backend/replication/libpqwalreceiver all || \
        { echo 'error: building static libpqwalreceiver test module' ; exit 39; }
    rm -f src/interfaces/libpq/*.o src/interfaces/libpq/libpq.a
    emmake make PORTNAME=emscripten \
        CPPFLAGS="-Dpg_link_canary_is_frontend=pgl_static_libpq_link_canary_is_frontend" \
        -C src/interfaces/libpq all || \
        { echo 'error: building libpq for the static walreceiver' ; exit 40; }
    STATIC_MODULE_ARGS+=(
        --module libpqwalreceiver \
            src/backend/replication/libpqwalreceiver/libpqwalreceiver.o
        --alias \
            libpqwalreceiver:_PG_init=pgl_static_libpqwalreceiver_PG_init
    )
    if [ "${PGLITE_POSTMASTER:-false}" = true ]; then
        # Install the already-built contrib archives into the test artifact so
        # PostgreSQL sees the normal control/SQL/library layout.  Execution is
        # claimed by the static registry before Emscripten's side-module
        # loader, keeping every server pointer in the transformed main module.
        for archive in "$INSTALL_FOLDER"/extensions/*.tar.gz; do
            [ -f "$archive" ] || continue
            tar -xzf "$archive" -C "$INSTALL_FOLDER" || {
                echo "error: installing static contrib archive $archive"
                exit 45
            }
        done

        build_static_pgxs_tree contrib false

        build_static_module src/backend/replication/pgoutput pgoutput \
            src/backend/replication/pgoutput/pgoutput.o
        # A clean backend build can have only a subset of Snowball's objects
        # materialized when the shell would expand *.o.  Ask its Makefile for
        # the complete configured object list before the discovery build so
        # every stemmer is copied into the static world-test artifact.
        snowball_objects=$(emmake make -s PORTNAME=emscripten \
            -C src/backend/snowball \
            --eval='pglite-print-objs: ; @printf "%s" "$(OBJS)"' \
            pglite-print-objs 2>/dev/null | tail -n 1)
        snowball_module_objects=()
        for object in $snowball_objects; do
            snowball_module_objects+=("src/backend/snowball/$object")
        done
        [ "${#snowball_module_objects[@]}" -gt 0 ] || {
            echo 'error: discovering configured Snowball objects'
            exit 46
        }
        build_static_module src/backend/snowball dict_snowball \
            "${snowball_module_objects[@]}"

        build_static_pgxs_tree src/test/modules true
    fi
    for module_dir in src/backend/utils/mb/conversion_procs/*; do
        [ -d "$module_dir" ] || continue
        module_name=$(basename "$module_dir")
        rm -f "$module_dir"/*.o "$module_dir"/*.so
        emmake make PORTNAME=emscripten \
            CPPFLAGS="-DPg_magic_func=pgl_static_${module_name}_Pg_magic_func" \
            -C "$module_dir" install || \
            { echo "error: building static $module_name conversion module" ; exit 35; }
        module_objects=("$module_dir"/*.o)
        [ -f "${module_objects[0]}" ] || continue
        STATIC_MODULE_ARGS+=(
            --module "$module_name" "${module_objects[@]}"
        )
    done
    node22 pglite/tests/postmaster/generate-static-test-modules.mjs \
        --output-dir "$STATIC_TEST_DIR" \
        --llvm-nm "$LLVM_NM" \
        "${STATIC_MODULE_ARGS[@]}" || \
        { echo 'error: generating static regression module registry' ; exit 36; }
    emcc $PGLITE_CFLAGS -c \
        "$STATIC_TEST_DIR/static-test-modules.c" \
        -o "$STATIC_TEST_DIR/static-test-modules.o" || \
        { echo 'error: compiling static regression module registry' ; exit 37; }
    emcc $PGLITE_CFLAGS -c \
        pglite/tests/postmaster/static-libpq-encoding.c \
        -o "$STATIC_TEST_DIR/static-libpq-encoding.o" || \
        { echo 'error: compiling the static libpq encoding adapter' ; exit 41; }
    PGLITE_STATIC_TEST_OBJECTS="$(tr '\n' ' ' < "$STATIC_TEST_DIR/objects.rsp") $STATIC_TEST_DIR/static-test-modules.o $STATIC_TEST_DIR/static-libpq-encoding.o $(pwd)/src/interfaces/libpq/libpq.a"
}

if [ "${PGLITE_BACKEND_ONLY:-false}" = true ]; then
    if [ "${PGLITE_CLEAN_BACKEND:-false}" = true ]; then
        emmake make PORTNAME=emscripten -C src/backend clean || exit 51
        emmake make PORTNAME=emscripten -C src/common clean || exit 52
        emmake make PORTNAME=emscripten -C src/port clean || exit 53
        # src/backend/utils/Makefile removes the generated inputs but leaves
        # this cross-directory stamp behind, so force its normal prerequisite
        # rule to materialize the headers and symlinks again.
        rm -f src/include/nodes/header-stamp
        rm -f src/include/utils/header-stamp
    fi
    emmake make PORTNAME=emscripten -C src/backend generated-headers || exit 54
    if [ "${PGLITE_POSTMASTER:-false}" = true ]; then
        emmake make PORTNAME=emscripten -C src/backend/libpq \
            auth.o || exit 64
        emmake make PORTNAME=emscripten -C src/backend/utils/fmgr \
            dfmgr.o || exit 61
        emmake make PORTNAME=emscripten -C src/backend/utils/misc \
            stack_depth.o || exit 62
        emmake make PORTNAME=emscripten -C src/backend/postmaster \
            checkpointer.o || exit 63
    fi
    if [ "${PGLITE_MULTI_MEMORY_PROVENANCE:-false}" = true ]; then
        emmake make PORTNAME=emscripten -C src/backend/executor \
            execExprInterp.o execTuples.o || exit 55
    fi
    # The direct pglite target consumes these archives but, unlike the normal
    # top-level build, has no submake rule that orders their construction.
    emmake make PORTNAME=emscripten -C src/port \
        -j"${PGLITE_BUILD_JOBS:-}" all || exit 56
    emmake make PORTNAME=emscripten -C src/common \
        -j"${PGLITE_BUILD_JOBS:-}" all || exit 57
    if [ "${PGLITE_WITH_REGRESSION_TESTS:-false}" = true ]; then
        build_regression_test_modules
    fi
    # /install is image-local, so rematerialize the Emscripten export list in
    # every backend-only container invocation from the persisted install tree.
    emmake make PORTNAME=emscripten -C src/backend \
        pglite-exported-functions || exit 58
    PGLITE_EXTRA_OBJS="${PGLITE_STATIC_TEST_OBJECTS:-}" \
    POSTGRES_PGLITE_FLAGS="$PGLITE_CFLAGS $POSTGRES_PGLITE_FLAGS ${PGLITE_STATIC_TEST_LIBS:-}" \
        emmake make PORTNAME=emscripten -C src/backend \
        -j"${PGLITE_BUILD_JOBS:-}" pglite || exit 59
    emmake make PORTNAME=emscripten -C src/backend install-pglite || exit 60
    exit 0
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
if [ "${PGLITE_POSTMASTER:-false}" = true ]; then
    # SUBDIROBJS tracks each directory's objfiles.txt rather than its member
    # objects.  Rebuild the fenced dynamic-loader boundary explicitly after
    # deleting it above, before the parallel top-level link can consume it.
    emmake make PORTNAME=emscripten -C src/backend generated-headers \
        || { echo 'error: generating headers for the postmaster loader boundary' ; exit 18; }
    emmake make PORTNAME=emscripten -C src/backend/libpq auth.o \
        || { echo 'error: rebuilding the postmaster peer-identity boundary' ; exit 26; }
    emmake make PORTNAME=emscripten -C src/backend/utils/fmgr dfmgr.o \
        || { echo 'error: rebuilding the postmaster loader boundary' ; exit 22; }
    emmake make PORTNAME=emscripten -C src/backend/utils/misc stack_depth.o \
        || { echo 'error: rebuilding postmaster stack-depth checks' ; exit 24; }
    emmake make PORTNAME=emscripten -C src/backend/postmaster checkpointer.o \
        || { echo 'error: rebuilding postmaster checkpoint routing' ; exit 25; }
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

# The upstream regression suite loads the core regress module directly through
# $libdir. Keep this opt-in so the ordinary PGlite artifact does not acquire
# test-only code, while still using the normal extension-import export path for
# the shared test build.
if [ "${PGLITE_WITH_REGRESSION_TESTS:-false}" = true ]; then
    build_regression_test_modules
fi

# Step 4: make and dist other extensions. The postmaster integration suite
# validates the core dependency/PostgreSQL/contrib world separately; dynamic
# third-party side modules remain separately gated.
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

# Building pglite itself needs to be the last step because of the PRELOAD_FILES parameter (a list of files and folders) need to be available.
PGLITE_EXTRA_OBJS="${PGLITE_STATIC_TEST_OBJECTS:-}" \
POSTGRES_PGLITE_FLAGS="$PGLITE_CFLAGS $POSTGRES_PGLITE_FLAGS ${PGLITE_STATIC_TEST_LIBS:-}" emmake make PORTNAME=emscripten -C src/backend/ -j"${PGLITE_BUILD_JOBS:-}" pglite || { echo 'emmake make OPTFLAGS="" PORTNAME=emscripten -j -C pglite' ; exit 61; }
emmake make PORTNAME=emscripten -C src/backend/ install-pglite || { echo 'emmake make PORTNAME=emscripten -C src/backend/ install-pglite' ; exit 62; }
