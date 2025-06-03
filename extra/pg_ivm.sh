#!/bin/bash

. wasm-build/extension.sh

pushd build
    if [ -d pg_ivm ]
    then
        echo using local pgpg_ivm
    else
        wget https://github.com/sraoss/pg_ivm/archive/refs/tags/v1.10.tar.gz -O-|tar xfz -
        mv pg_ivm-* pg_ivm
        #git clone --recursive --no-tags --depth 1 --single-branch --branch main https://github.com/sraoss/pg_ivm
    fi
popd

pushd build/pg_ivm
    # path for wasm-shared already set to (pwd:pg build dir)/bin
    # OPTFLAGS="" turns off arch optim (sse/neon).
    PG_CONFIG=${PGROOT}/bin/pg_config emmake make OPTFLAGS="" install || exit 19

    #cp sql/pg_ivm--1.10.sql ${PGROOT}/share/postgresql/extension/
    #rm -f ${PGROOT}/share/postgresql/extension/pg_ivm--?.?.?--?.?.?.sql ${PGROOT}/share/postgresql/extension/pg_ivm.sql

popd


