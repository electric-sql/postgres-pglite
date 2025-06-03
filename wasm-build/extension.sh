# this is to be sourced by extra/*sh

mkdir -p build src

if which emcc
then
    echo -n
else
    reset;
    . ${SDKROOT:-/tmp/sdk}/wasm32-bi-emscripten-shell.sh
    export PGROOT=${PGROOT:-/tmp/pglite}
    export PATH=${PGROOT}/bin:$PATH
    . ${PGROOT}/pgopts.sh
fi
