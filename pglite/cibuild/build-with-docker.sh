#!/bin/bash
echo "======== build-with-dockerl.sh : $(pwd)                 =========="
echo "======== Building all PGlite prerequisites using Docker =========="

trap 'echo caught interrupt and exiting;' INT

source ./pglite/.buildconfig

if [[ -z "$SDK_VERSION" || -z "$PG_VERSION" ]]; then
  echo "Missing SDK_VERSION and PG_VERSION env vars."
  echo "Source them from .buildconfig"
  exit 1
fi

IMG_NAME="electricsql/emsdk"
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
  bash ./cibuild/build-all.sh