# although we could use any path inside docker, using the same path as on the host
# allows the DWARF info (when building in DEBUG) to contain the correct file paths
DOCKER_WORKSPACE=$(pwd)
echo "PGLITE_VERSION=${PGLITE_VERSION}"
echo "DEBUG=${DEBUG}"
  
# ./build-pglite.sh (default) or ./build-pglite-standalone.sh
BUILD_SCRIPT=${BUILD_SCRIPT:-./build-pglite.sh}
# docker (default) or podman
CONTAINER_RUNNER=${CONTAINER_RUNNER:-docker}
${CONTAINER_RUNNER} run $@ \
  --rm \
  -e DEBUG=${DEBUG:-false} \
  -e PGLITE_VERSION=${PGLITE_VERSION} \
  --workdir=${DOCKER_WORKSPACE} \
  -v .:${DOCKER_WORKSPACE}:rw \
  -v ./dist:/pglite:rw \
  docker.io/electricsql/pglite-builder:6.0.13-3  \
  ${BUILD_SCRIPT}
