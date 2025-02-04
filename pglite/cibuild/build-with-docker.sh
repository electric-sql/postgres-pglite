#!/bin/bash
echo "======== build-with-docker.sh : $(pwd)                 =========="

# these are all the elements that can be build as part of this project
ALL="contrib extra node linkweb postgres-pglite-dist"

# this is what we will actually build
WHAT=${*:-$ALL}

echo "======== Building PGlite prerequisites ${WHAT} using Docker =========="

# trap 'echo caught interrupt and exiting;' INT

source ./pglite/.buildconfig

if [[ -z "$SDK_VERSION" || -z "$PG_VERSION" ]]; then
  echo "Missing SDK_VERSION and PG_VERSION env vars."
  echo "Source them from .buildconfig"
  exit 1
fi

# we are using a custom emsdk to build pglite wasm
# this is available as a docker image under electricsql/pglite-builder
IMG_NAME="electricsql/pglite-builder"
IMG_TAG="${PG_VERSION}_${SDK_VERSION}"

docker run \
  --rm \
  -e OBJDUMP=${OBJDUMP:-true} \
  -e PGSRC=/workspace/postgres-src \
  -e POSTGRES_PGLITE_OUT=/workspace/dist \
  -v ./pglite/cibuild.sh:/workspace/cibuild.sh:rw \
  -v ./pglite/.buildconfig:/workspace/.buildconfig:rw \
  -v ./pglite/extra:/workspace/extra:rw \
  -v ./pglite/cibuild:/workspace/cibuild:rw \
  -v ./pglite/patches:/workspace/patches:rw \
  -v ./pglite/tests:/workspace/tests:rw \
  -v .:/workspace/postgres-src \
  -v ./pglite/dist:/workspace/dist \
  $IMG_NAME:$IMG_TAG \
  bash /workspace/cibuild.sh $WHAT