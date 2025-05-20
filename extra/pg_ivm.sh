#!/bin/bash

mkdir -p build src

pushd build
    if [ -d pg_ivm ]
    then
        echo using local pgpg_ivm
    else
        git clone --recursive --no-tags --depth 1 --single-branch --branch main https://github.com/sraoss/pg_ivm
    fi
popd



if which emcc
then
    echo -n
else
    reset;
    . ${SDKROOT}/wasm32-bi-emscripten-shell.sh
    export PGROOT=${PGROOT:-/tmp/pglite}
    export PATH=${PGROOT}/bin:$PATH
fi



pushd build/pg_ivm
    # path for wasm-shared already set to (pwd:pg build dir)/bin
    # OPTFLAGS="" turns off arch optim (sse/neon).
    PG_CONFIG=${PGROOT}/bin/pg_config emmake make OPTFLAGS="" install || exit 33

    #cp sql/pg_ivm--1.10.sql ${PGROOT}/share/postgresql/extension/
    #rm -f ${PGROOT}/share/postgresql/extension/pg_ivm--?.?.?--?.?.?.sql ${PGROOT}/share/postgresql/extension/pg_ivm.sql

popd


