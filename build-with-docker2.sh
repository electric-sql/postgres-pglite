# we are using a custom emsdk to build pglite wasm
# this is available as a docker image under electricsql/pglite-builder
IMG_NAME=${IMG_NAME:-"electricsql/pglite-builder"}
IMG_TAG=${IMG_TAG:-"3.1.74_vanilla"}

DOCKER_WORKSPACE=/src/postgres-pglite
DEBUG_SOURCE_PATH=$(pwd)

docker run $@ \
  --rm \
  -e DEBUG=${DEBUG:-false} \
  --workdir=${DOCKER_WORKSPACE} \
  -v .:${DOCKER_WORKSPACE}:rw \
  -v ./dist:/install/pglite:rw \
  $IMG_NAME:$IMG_TAG \
  ./build-pglite.sh
  
