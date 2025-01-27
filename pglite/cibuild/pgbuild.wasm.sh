echo "



pgbuild.wasm:begin

CC_PGLITE=$CC_PGLITE

"


    mkdir -p build/postgres
    pushd build/postgres

    # create empty package.json to avoid emsdk node conflicts
    # with root package.json of project
    echo "{}" > package.json

    export MAIN_MODULE="-sMAIN_MODULE=1"
    export XML2_CONFIG=$PREFIX/bin/xml2-config
    export ZIC=$(pwd)/bin/zic

    cp ${PGSRC}/./src/include/port/wasm_common.h /tmp/pglite/include/wasm_common.h

    CNF="${PGSRC}/configure FLEX=`which flex` --prefix=/tmp/pglite \
 --cache-file=/tmp/pglite/config.cache.emscripten \
 --disable-spinlocks --disable-largefile --without-llvm \
 --without-pam --disable-largefile --with-openssl=no \
 --without-readline --without-icu \
 --with-uuid=ossp \
 --with-zlib --with-libxml --with-libxslt \
  ${PGDEBUG}"

    echo "  ==== building wasm MVP:$MVP Debug=${PGDEBUG} with opts : $@  == "

    mkdir -p bin

    [ -f /usr/bin/zic ] && cp /usr/bin/zic bin/
    if [ -f bin/zic ]
    then
        echo "using system zic"
        GETZIC=false
    else
        echo "getting zic"
        GETZIC=true
    fi

    export EXT=wasm
    ACCVEXEEXT=.cjs

    if $GETZIC
    then
        cat > bin/zic <<END
#!/bin/bash
#. /opt/python-wasm-sdk/wasm32-bi-emscripten-shell.sh
TZ=UTC PGTZ=UTC $(which node) $(pwd)/src/timezone/zic.cjs \$@
END
    fi

    if EM_PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig ac_cv_exeext=$ACCVEXEEXT emconfigure $CNF --with-template=emscripten
    then
        echo configure ok
    else
        echo configure failed
        exit 76
    fi

    if grep -q MAIN_MODULE ${PGSRC}/src/backend/Makefile
    then
        echo "dyld server patch ok"
    else
        echo "missing server dyld patch"
        exit 273
    fi

    > /tmp/disable-shared.log

    mkdir -p /tmp/pglite/bin

    cat > /tmp/pglite/bin/emsdk-shared <<END
#!/bin/bash
echo "[\$(pwd)] $0 \$@" >> /tmp/disable-shared.log
# shared build
\${PG_LINK:-emcc} -L${PREFIX}/lib -DPREFIX=/tmp/pglite -shared -sSIDE_MODULE=1 \$@ -Wno-unused-function
END
    ln -sf /tmp/pglite/bin/emsdk-shared bin/emsdk-shared

    chmod +x bin/zic /tmp/pglite/bin/emsdk-shared

    # for zic and emsdk-shared/wasi-shared called from makefile
    export PATH=$(pwd)/bin:$PATH

    EMCC_NODE="-sEXIT_RUNTIME=1 -DEXIT_RUNTIME -sNODERAWFS -sENVIRONMENT=node"

    EMCC_ENV="${EMCC_NODE} -sFORCE_FILESYSTEM=0"
    EMCC_ENV="${EMCC_NODE} -sERROR_ON_UNDEFINED_SYMBOLS"

    # PREFIX only required for static initdb
    EMCC_CFLAGS="-sERROR_ON_UNDEFINED_SYMBOLS=1 ${CC_PGLITE} -DPREFIX=/tmp/pglite -Wno-macro-redefined -Wno-unused-function"

    ZIC=${ZIC:-$(realpath bin/zic)}

	if EMCC_CFLAGS="${EMCC_ENV} ${EMCC_CFLAGS}" WASI_CFLAGS="$WASI_CFLAGS" emmake make emscripten=1 -j $(nproc) 2>&1 > /tmp/build.log
	then
        echo build ok
        cp -vf src/backend/postgres src/backend/postgres.cjs

        # if running a 32bits zic from current build
        unset LD_PRELOAD

        if EMCC_CFLAGS="${EMCC_ENV} ${EMCC_CFLAGS}" WASI_CFLAGS="$WASI_CFLAGS" emmake make emscripten=1 install 2>&1 > /tmp/install.log
        then
            echo install ok
            pushd /tmp/pglite

            find . -type f | grep -v plpgsql > /tmp/pglite/pg.installed
            popd

            goback=$(pwd)
            popd
            python3 cibuild/pack_extension.py builtin
            pushd $goback

            pushd /tmp/pglite
            find . -type f  > /tmp/pglite/pg.installed
            popd

        else
            cat /tmp/install.log
            echo "install failed"
            exit 225
        fi
    else
        cat /tmp/build.log
        echo "build failed"
        exit 230
	fi

    # wip
    mv -vf ./src/bin/psql/psql.$EXT ./src/bin/pg_config/pg_config.$EXT /tmp/pglite/bin/
    mv -vf ./src/bin/pg_dump/pg_restore.$EXT ./src/bin/pg_dump/pg_dump.$EXT ./src/bin/pg_dump/pg_dumpall.$EXT /tmp/pglite/bin/
	mv -vf ./src/bin/pg_resetwal/pg_resetwal.$EXT  ./src/bin/initdb/initdb.$EXT ./src/backend/postgres.$EXT /tmp/pglite/bin/


    cat > /tmp/pglite/PGPASSFILE <<END
USER="${PGPASS:-postgres}"
PASS="${PGUSER:-postgres}"
md5pass =  "md5" + __import__('hashlib').md5(USER.encode() + PASS.encode()).hexdigest()
print(f"localhost:5432:postgres:{USER}:{md5pass}")

USER="postgres"
PASS="postgres"
md5pass =  "md5" + __import__('hashlib').md5(USER.encode() + PASS.encode()).hexdigest()
print(f"localhost:5432:postgres:{USER}:{md5pass}")

USER="login"
PASS="password"
md5pass =  "md5" + __import__('hashlib').md5(USER.encode() + PASS.encode()).hexdigest()
print(f"localhost:5432:postgres:{USER}:{md5pass}")
END


    if [ -f /tmp/pglite/bin/pg_config.$EXT ]
    then
        echo pg_config installed
    else
        echo "pg_config build failed"; exit 243
    fi

    cat > /tmp/pglite/bin/pg_config <<END
#!/bin/bash
$(which node) /tmp/pglite/bin/pg_config.cjs \$@
END

    cat  > /tmp/pglite/postgres <<END
#!/bin/bash
. /opt/python-wasm-sdk/wasm32-bi-emscripten-shell.sh
TZ=UTC PGTZ=UTC PGDATA=${PGDATA} $(which node) /tmp/pglite/bin/postgres.cjs \$@
END

    # force node wasm version
    cp -vf /tmp/pglite/postgres /tmp/pglite/bin/postgres

	cat  > /tmp/pglite/initdb <<END
#!/bin/bash
. /opt/python-wasm-sdk/wasm32-bi-emscripten-shell.sh
TZ=UTC PGTZ=UTC $(which node) /tmp/pglite/bin/initdb.cjs \$@
END

    chmod +x /tmp/pglite/postgres /tmp/pglite/bin/postgres
	chmod +x /tmp/pglite/initdb /tmp/pglite/bin/initdb

    # for extensions building
    chmod +x /tmp/pglite/bin/pg_config

    popd
echo "pgbuild:end




"