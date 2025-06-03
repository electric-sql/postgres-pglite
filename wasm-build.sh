#!/bin/bash
export PG_VERSION=${PG_VERSION:-17.4}

#set -x;
#set -e;
export LC_ALL=C

export CI=${CI:-false}
export PORTABLE=${PORTABLE:-$(pwd)/wasm-build}
export SDKROOT=${SDKROOT:-/tmp/sdk}

export GETZIC=${GETZIC:-true}
# systems default may not be in path
export ZIC=${ZIC:-/usr/sbin/zic}

# data transfer zone this is == (wire query size + result size ) + 2
# expressed in EMSDK MB, max is 13MB on emsdk 3.1.74+
export CMA_MB=${CMA_MB:-12}
export TOTAL_MEMORY=${TOTAL_MEMORY:-180MB}


export WORKSPACE=${GITHUB_WORKSPACE:-$(pwd)}
export PGROOT=${PGROOT:-/tmp/pglite}
export WEBROOT=${WEBROOT:-/tmp/web}

export PG_BUILD=${BUILD:-/tmp/sdk/build}
export PGL_BUILD_NATIVE="${PG_BUILD}/pglite-native"

export PG_DIST=${DIST:-/tmp/sdk/dist}
export PG_DIST_EXT="${PG_DIST}/extensions-emsdk"

export PGL_DIST_JS="${PG_DIST}/pglite-js"
export PGL_DIST_LINK="${PG_DIST}/pglite-link"

export PGL_DIST_NATIVE="${PG_DIST}/pglite-native"
export PGL_DIST_C="${PG_DIST}/pglite-native"
export PGL_DIST_WEB="${PG_DIST}/pglite-web"

DIST_ALL="${PGROOT}/bin ${PG_DIST} ${PG_DIST_EXT} ${PGL_DIST_JS} ${PGL_DIST_LINK} ${PGL_DIST_WEB} ${PGL_BUILD_NATIVE}"

export DEBUG=${DEBUG:-true}

export USE_ICU=${USE_ICU:-false}
export PGUSER=${PGUSER:-postgres}

[ -f /tmp/portable.opts ] && . /tmp/portable.opts
[ -f /tmp/portable.dev ] && . /tmp/portable.dev

# can override from cmd line
export WASI=${WASI:-false}
export WASI_SDK=${WASI_SDK:-25.0}
export PYBUILD=${PYBUILD:-3.13}
export NATIVE=${NATIVE:-false}


if $WASI
then
    BUILD=wasi
    if $DEBUG
    then
        export COPTS=${COPTS:-"-O2 -g3"}
        export LOPTS=${LOPTS:-"-O2 -g3"}
    else
        export COPTS=${COPTS:-"-Oz -g0"}
        export LOPTS=${LOPTS:-"-Oz -g0"}
    fi
else
    if grep -q __emscripten_tempret_get ${SDKROOT}/emsdk/upstream/emscripten/src/library_dylink.js
    then
        echo -n
    else
        pushd ${SDKROOT}/emsdk
        patch -p1 <<END
--- emsdk/upstream/emscripten/src/library_dylink.js
+++ emsdk.fix/upstream/emscripten/src/library_dylink.js
@@ -724,6 +724,8 @@
             stubs[prop] = (...args) => {
               resolved ||= resolveSymbol(prop);
               if (!resolved) {
+                if (prop==='getTempRet0')
+                    return __emscripten_tempret_get(...args);
                 throw new Error(\`Dynamic linking error: cannot resolve symbol \${prop}\`);
               }
               return resolved(...args);
END
        popd
    fi

    BUILD=emscripten
    if $DEBUG
    then
        # clang default to O0 but specifying -O0 may trigger align bug in emsdk
        if [ -f /alpine ]
        then
            # dev debug
            export COPTS="-O2 -g3 --no-wasm-opt"
            export LOPTS=${LOPTS:-"-O2 -g3 --no-wasm-opt -sASSERTIONS=1"}
        else
            # docker debug ( exepected to be ide friendly )
            export COPTS="-g3 --no-wasm-opt"
            export LOPTS=${LOPTS:-"-g3 --no-wasm-opt -sASSERTIONS=1"}
        fi
    else
        # DO NOT CHANGE COPTS - optimized wasm corruption fix
        export COPTS="-O2 -g3 --no-wasm-opt"
        export LOPTS=${LOPTS:-"-Os -g0 --closure=0 -sASSERTIONS=0"}
    fi
fi

export BUILD
export BUILD_PATH=${PG_BUILD}/${BUILD}

export PGDATA=${PGROOT}/base
export PGPATCH=${WORKSPACE}/patches

chmod +x ${PORTABLE}/*.sh
[ -d ${PORTABLE}/extra ] && ${PORTABLE}/extra/*.sh


# this was set to false on 16.4 to skip some harmless exceptions without messing with core code.
# exit on error
EOE=true


# default to user writeable paths in /tmp/ .
if mkdir -p $DIST_ALL
then
    echo "checking for valid prefix ${PGROOT} ${PG_DIST}"
else
    sudo mkdir -p $DIST_ALL
    sudo chown $(whoami) -R $DIST_ALL
fi

# TODO: also handle PGPASSFILE hostname:port:database:username:password
# https://www.postgresql.org/docs/devel/libpq-pgpass.html
export CRED="-U $PGUSER --pwfile=${PGROOT}/password"

if [ -f ${PGROOT}/password ]
then
    echo "not changing db password"
    PGPASS=$(cat ${PGROOT}/password)
else
    PGPASS=${PGPASS:-password}
    echo ${PGPASS:-password} > ${PGROOT}/password
fi

export PGPASS


export PG_DEBUG_HEADER="${PGROOT}/include/pg_debug.h"


echo "

System node/pnpm ( may interfer) :

        node : $(which node) $(which node && $(which node) -v)
        PNPM : $(which pnpm)


"



# setup compiler+node. emsdk provides node 20, recent enough for bun.
# TODO: but may need to adjust $PATH with stock emsdk.

if ${WASI}
then
    pushd ${SDKROOT}
     . wasisdk/wasisdk_env.sh
    popd

    if [ -f ${WASI_SYSROOT}/extra ]
    then
        echo -n
    else
        pushd ${WASI_SYSROOT}
            VMLABS="https://github.com/vmware-labs/webassembly-language-runtimes/releases/download"
            wget -q "${VMLABS}/libs%2Flibpng%2F1.6.39%2B20230629-ccb4cb0/libpng-1.6.39-wasi-sdk-20.0.tar.gz" -O-| tar xfz -
            wget -q "${VMLABS}/libs%2Fzlib%2F1.2.13%2B20230623-2993864/libz-1.2.13-wasi-sdk-20.0.tar.gz"  -O-| tar xfz -
            wget -q "${VMLABS}/libs%2Fsqlite%2F3.42.0%2B20230623-2993864/libsqlite-3.42.0-wasi-sdk-20.0.tar.gz" -O-| tar xfz -
            wget -q "${VMLABS}/libs%2Flibxml2%2F2.11.4%2B20230623-2993864/libxml2-2.11.4-wasi-sdk-20.0.tar.gz" -O-| tar xfz -
            wget -q "${VMLABS}/libs%2Fbzip2%2F1.0.8%2B20230623-2993864/libbzip2-1.0.8-wasi-sdk-20.0.tar.gz"  -O-| tar xfz -
            wget -q "${VMLABS}/libs%2Flibuuid%2F1.0.3%2B20230623-2993864/libuuid-1.0.3-wasi-sdk-20.0.tar.gz" -O-| tar xfz -
        popd
        touch ${WASI_SYSROOT}/extra
    fi

    # wasi does not use -sGLOBAL_BASE
    CC_PGLITE="-DCMA_MB=${CMA_MB}"

else
    if which emcc
    then
        echo "emcc found in PATH=$PATH"
    else
        if ${PORTABLE}/sdk.sh
        then
            echo "$PORTABLE : sdk check passed (emscripten)"
        else
            echo emsdk failed
            exit 150
        fi

        . ${SDKROOT}/wasm32-bi-emscripten-shell.sh
    fi
    export PG_LINK=${PG_LINK:-$(which emcc)}

    echo "

    Using provided emsdk from $(which emcc)
    Using PG_LINK=$PG_LINK as linker

        node : $(which node) $($(which node) -v)
        PNPM : $(which pnpm)


"

    # pass the "kernel" contiguous memory zone size to the C compiler.
    CC_PGLITE="-DCMA_MB=${CMA_MB}"

fi

# also used for not makefile (manual linking and pgl_main)
export CC_PGLITE="-DPYDK=1 -DPG_PREFIX=${PGROOT} -I${PGROOT}/include ${CC_PGLITE}"



echo "
    ----------------------------------------
"
env|grep PG |grep -v BUILD
env|grep BUILD|grep -v PG
env|grep WA
env|grep PY

echo "
    ----------------------------------------
"




# ========================= symbol extractor ============================

OBJDUMP=${OBJDUMP:-true}

if $OBJDUMP
then
    if [ -f $PGROOT/bin/wasm-objdump ]
    then
        echo "wasm-objdump found"
    else
        WRAPPER=$(which wasm-objdump)
        WASIFILE=$(realpath ${WRAPPER}.wasi)
        if $WRAPPER -h $WASIFILE | grep -q 'file format wasm 0x1'
        then
            mkdir -p $PGROOT/bin/
            if cp -f $WRAPPER $WASIFILE $PGROOT/bin/
            then
                echo "wasm-objdump found and working, and copied to $PGROOT/bin/"
            else
                OBJDUMP=false
            fi
        else
            echo "
        WARNING: $(which wasm-objdump) not working properly, trying alternate syntax

    "
            cat > $WRAPPER <<END
#!/bin/bash
LNK="\$(realpath \$0).wasi"
if [ -f "\$LNK" ]
then
    WASM=\$LNK
else
    WASM=\$1
    shift
    if [ -f "\${WASM}.wasi" ]
    then
        WASM="\${WASM}.wasi"
    fi
fi
echo "WASI: \$WASM \$@" > /proc/self/fd/2
$(which wasmtime) --env PYTHONDONTWRITEBYTECODE=1 --dir / \$WASM \$@
END
            chmod +x $WRAPPER

            if $WRAPPER -h $WASIFILE | grep -q 'file format wasm 0x1'
            then
                mkdir -p $PGROOT/bin/
                if cp -f $WRAPPER $WASIFILE $PGROOT/bin/
                then
                    echo "wasm-objdump fixed and working, copied to $PGROOT/bin/"
                else
                    OBJDUMP=false
                fi
            else
                echo "
        ERROR: $(which wasm-objdump) not working properly ( is wasmtime ok ? )

    "
                exit 268
                OBJDUMP=false
            fi
        fi
    fi
else
    echo "

    WARNING: OBJDUMP disabled, some newer or complex extensions may not load properly


"
fi

if $OBJDUMP
then
    mkdir -p patches/imports patches/imports.pgcore
else
    mkdir -p patches/imports
    touch patches/imports/plpgsql
    echo "

    WARNING:    wasm-objdump not found or OBJDUMP disabled, some extensions may not load properly


"
fi

export OBJDUMP


# ========================= pg core configuration ============================

# testing postgres.js file instead of ${PGROOT}/pgopts.sh because build should not have failed.
if [ -f ${WEBROOT}/postgres.js ]
then
    echo using current from ${WEBROOT}

    . ${PGROOT}/pgopts.sh

else

    # default to web/release size optim.

    mkdir -p ${PGROOT}/include
    if $DEBUG
    then
        export PGDEBUG=""
        cat > ${PG_DEBUG_HEADER} << END
#ifndef I_PGDEBUG
#define I_PGDEBUG
#define WASM_USERNAME "$PGUSER"
#define PGDEBUG 1
#define PDEBUG(string) fputs(string, stderr)
#define JSDEBUG(string) {EM_ASM({ console.log(string); });}
#define ADEBUG(string) { PDEBUG(string); JSDEBUG(string) }
#endif
END

    else
        export PGDEBUG=""
        cat > ${PG_DEBUG_HEADER} << END
#ifndef I_PGDEBUG
#define I_PGDEBUG
#define WASM_USERNAME "$PGUSER"
#define PDEBUG(string)
#define JSDEBUG(string)
#define ADEBUG(string)
#define PGDEBUG 0
#endif
END
    fi

    mkdir -p ${PGROOT}/include/postgresql/server
    for dest in ${PGROOT}/include ${PGROOT}/include/postgresql ${PGROOT}/include/postgresql/server
    do
        [ -f $dest/pg_debug.h ] || cp ${PG_DEBUG_HEADER} $dest/
    done

    # store all options that have impact on cmd line initdb/boot compile+link
    cat > ${PGROOT}/pgopts.sh <<END
export PG_BRANCH=$PG_BRANCH
export CMA_MB=$CMA_MB
export TOTAL_MEMORY=$TOTAL_MEMORY
export CC_PGLITE="$CC_PGLITE"

export CI=$CI
export PORTABLE=$PORTABLE
export SDKROOT=$SDKROOT
export GETZIC=$GETZIC
export ZIC=$ZIC
export WORKSPACE=$WORKSPACE
export PGROOT=$PGROOT
export WEBROOT=$WEBROOT
export PG_BUILD=$PG_BUILD
export PGL_BUILD_NATIVE=$PGL_BUILD_NATIVE
export PG_DIST=$PG_DIST
export PG_DIST_EXT=$PG_DIST_EXT
export PGL_DIST_JS=$PGL_DIST_JS
export PGL_DIST_LINK=$PGL_DIST_LINK

export PGL_DIST_NATIVE=$PGL_DIST_NATIVE
export PGL_DIST_WEB=$PGL_DIST_WEB
export DEBUG=$DEBUG

export USE_ICU=$USE_ICU
export PGUSER=$PGUSER

export WASI=$WASI
export WASI_SDK=$WASI_SDK
export PYBUILD=$PYBUILD

export BUILD_PATH=$BUILD_PATH
export COPTS="$COPTS"
export LOPTS="$LOPTS"
export PGDEBUG="$PGDEBUG"
export PG_DEBUG_HEADER=$PG_DEBUG_HEADER
export PGOPTS="\\
 -c log_checkpoints=false \\
 -c dynamic_shared_memory_type=posix \\
 -c search_path=pg_catalog \\
 -c exit_on_error=$EOE \\
 -c ignore_invalid_pages=on \\
 -c temp_buffers=8MB -c work_mem=4MB \\
 -c fsync=on -c synchronous_commit=on \\
 -c wal_buffers=4MB -c min_wal_size=80MB \\
 -c shared_buffers=128MB"

END

    export PGLITE=$(pwd)/packages/pglite

    echo "export PGSRC=${WORKSPACE}" >> ${PGROOT}/pgopts.sh
    echo "export PGLITE=${PGLITE}" >> ${PGROOT}/pgopts.sh

    [ -f /tmp/portable.opts ] && cat /tmp/portable.opts >> ${PGROOT}/pgopts.sh
    [ -f /tmp/portable.dev ] && cat /tmp/portable.dev >> ${PGROOT}/pgopts.sh

    . ${PGROOT}/pgopts.sh

    # make sure no non-mvp feature gets in.
    cat > ${PGROOT}/config.site <<END
pgac_cv_sse42_crc32_intrinsics_=no
pgac_cv_sse42_crc32_intrinsics__msse4_2=no
pgac_sse42_crc32_intrinsics=no
pgac_armv8_crc32c_intrinsics=no
ac_cv_search_sem_open=no

with_uuid=ossp
ac_cv_lib_ossp_uuid_uuid_export=yes
ac_cv_lib_uuid_uuid_generate=no
END


    # workaround no "locale -a" for Node.
    # this is simply the minimal result a popen call would give.
    mkdir -p ${PGROOT}/etc/postgresql
    cat > ${PGROOT}/etc/postgresql/locale <<END
C
C.UTF-8
POSIX
UTF-8
END

    . ${PORTABLE}/build-pgcore.sh
fi

# put local zic in the path from build dir
# put emsdk-shared and also pg_config from the install dir.
export PATH=${WORKSPACE}/${BUILD_PATH}/bin:${PGROOT}/bin:$PATH


# At this stage, PG should be installed to PREFIX and ready for linking
# or building ext.



# ===========================================================================
# ===========================================================================
#                             EXTENSIONS
# ===========================================================================
# ===========================================================================

if true
then
    mkdir -p ${PGL_DIST_LINK}/dumps ${PGL_DIST_LINK}/imports

    if $WASI
    then
        echo " ========= TODO WASI openssl ============== "
        SKIP="\
 [\
 sslinfo bool_plperl hstore_plperl hstore_plpython jsonb_plperl jsonb_plpython\
 ltree_plpython sepgsql bool_plperl start-scripts\
 pgcrypto uuid-ossp xml2\
 ]"
    else
        # TEMP FIX for SDK
        SSL_INCDIR=$EMSDK/upstream/emscripten/cache/sysroot/include/openssl
        [ -f $SSL_INCDIR/evp.h ] || ln -s $PREFIX/include/openssl $SSL_INCDIR
        SKIP="\
 [\
 sslinfo bool_plperl hstore_plperl hstore_plpython jsonb_plperl jsonb_plpython\
 ltree_plpython sepgsql bool_plperl start-scripts\
 ]"
    fi

    for extdir in postgresql-${PG_BRANCH}/contrib/*
    do
        if [ -f ${PGL_DIST_LINK}/dumps/dump.vector ]
        then
            echo "

    *   NOT rebuilding extensions

"
            break
        fi

        if [ -d "$extdir" ]
        then
            ext=$(echo -n $extdir|cut -d/ -f3)
            if echo -n $SKIP|grep -q "$ext "
            then
                echo skipping extension $ext
            else
                echo "

        Building contrib extension : $ext : begin
"
                pushd ${BUILD_PATH}/contrib/$ext
                if PATH=$PREFIX/bin:$PATH emmake make install 2>&1 >/dev/null
                then
                    echo "
        Building contrib extension : $ext : end
"
                else
                    echo "

        Extension $ext from $extdir failed to build

"
                    exit 216
                fi
                popd

                python3 ${PORTABLE}/pack_extension.py 2>&1 >/dev/null

            fi
        fi
    done


fi


echo "

        Extensions distribution folder : ${PG_DIST_EXT}


"



# only build extra when targeting pglite-wasm .

if [ -f ${WORKSPACE}/pglite-${PG_BRANCH}/build.sh ]
then
    if $WASI
    then
        echo "
    * WASI build : skipping extra extensions and FS
"
    else

        if echo " $*"|grep -q " extra"
        then
#            if $CI
#            then
#                #if [ -d $PREFIX/include/X11 ]
#                if true
#                then
#                    echo -n
#                else
#                    # install EXTRA sdk
#                    . /etc/lsb-release
#                    DISTRIB="${DISTRIB_ID}-${DISTRIB_RELEASE}"
#                    CIVER=${CIVER:-$DISTRIB}
#                    SDK_URL=https://github.com/pygame-web/python-wasm-sdk-extra/releases/download/$SDK_VERSION/python3.13-emsdk-sdk-extra-${CIVER}.tar.lz4
#                    echo "Installing extra lib from $SDK_URL"
#                    curl -sL --retry 5 $SDK_URL | tar xvP --use-compress-program=lz4 | pv -p -l -s 15000 >/dev/null
#                    chmod +x ./extra/*.sh
#                fi
#            fi

            for extra_ext in ./extra/*.sh
            do
                LOG=$PG_DIST_EXT/$(basename ${extra_ext}).log
                echo "======================= ${extra_ext} : $LOG ==================="

                ${extra_ext} >  $LOG || exit 552

                python3 wasm-build/pack_extension.py
            done
        fi

        # this is for initial emscripten MEMFS
        export PGPRELOAD="\
--preload-file ${PGROOT}/share/postgresql@${PGROOT}/share/postgresql \
--preload-file ${PGROOT}/lib/postgresql@${PGROOT}/lib/postgresql \
--preload-file ${PGROOT}/password@${PGROOT}/password \
--preload-file ${PGROOT}/PGPASSFILE@/home/web_user/.pgpass \
--preload-file placeholder@${PGROOT}/bin/postgres \
--preload-file placeholder@${PGROOT}/bin/initdb\
"
    fi

    echo "
    * building + linking pglite-wasm (initdb/loop/transport/repl/backend)
"

    if $WASI
    then
        echo "TODO: wasi pack/tests"
    else
        cat > pglite-link.sh <<END
. ${PGROOT}/pgopts.sh
. ${SDKROOT}/wasm32-bi-emscripten-shell.sh
if ./pglite-${PG_BRANCH}/build.sh
then
    if [ -d pglite ]
    then
        mkdir -p pglite/packages/pglite/release

        for archive in ${PG_DIST_EXT}/*.tar
        do
            echo "    packing extension \$archive"
            gzip -f -k -9 \$archive
            mv \$archive.gz pglite/packages/pglite/release/
        done

        cp ${PGL_DIST_WEB}/pglite.* pglite/packages/pglite/release/
        pushd pglite
            export HOME=$PG_BUILD
            export PNPM_HOME=$PG_BUILD
            export PATH=$(echo -n ${SDKROOT}/emsdk/node/*.*.*/bin):$PNPM_HOME:$PATH
            which pnpm || npm install -g pnpm
            pnpm install -g npm vitest
            pnpm install
            pnpm run ts:build
        popd

        if [ -f /skiptest ]
        then
            echo skipping tests
        else
            if $CI
            then
                ./runtests.sh || exit 580
            fi
        fi
    fi
else
    echo "pglite linking failed"
    exit 657
fi
END
        chmod +x pglite-link.sh

        if ./pglite-link.sh
        then
            if [ -d pglite ]
            then
               echo -n
            else
                for archive in ${PG_DIST_EXT}/*.tar
                do
                    echo "    packing extension $archive (docker build)"
                    gzip -f -k -9 $archive
                done
            fi
        else
            exit 676
        fi
    fi
else
    echo "linking libpglite skipped"
fi
