
PG_VERSION=16.4
PG_PREREL=false

# these are files that shadow original portion of pg core, with minimal changes
# to original code
# some may be included multiple time
CC_PGLITE="-DPATCH_MAIN=${PGSRC}/src/pglite-extra/pg_main.c ${CC_PGLITE}"
CC_PGLITE="-DPATCH_LOOP=${PGSRC}/src/pglite-extra/interactive_one.c ${CC_PGLITE}"
CC_PGLITE="-DPATCH_PLUGIN=${PGSRC}/src/pglite-extra/pg_plugin.h ${CC_PGLITE}"

export CC_PGLITE

rm postgresql 2>/dev/null
ln -s $PGSRC postgresql

echo "Building postgresql-${PG_VERSION} in folder $PGSRC"
if ${WASI}
then
    . cibuild/pgbuild.sh
else
    . cibuild/pgbuild.emscripten.sh
fi