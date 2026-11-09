# although we could use any path inside docker, using the same path as on the host
# allows the DWARF info (when building in DEBUG) to contain the correct file paths
DOCKER_WORKSPACE=$(pwd)
echo "PGLITE_VERSION=${PGLITE_VERSION}"
echo "DEBUG=${DEBUG}"

# optional out-of-tree build folder (see build-pglite.sh). For the same reason as above,
# it is mounted at the same path inside docker as on the host.
BUILD_DIR_ARGS=""
if [ -n "${BUILD_DIR}" ]; then
  # create it upfront, otherwise docker creates it owned by root; also makes the path absolute
  BUILD_DIR=$(mkdir -p "${BUILD_DIR}" && cd "${BUILD_DIR}" && pwd) || exit 1
  BUILD_DIR_ARGS="-e BUILD_DIR=${BUILD_DIR} -v ${BUILD_DIR}:${BUILD_DIR}:rw"
fi
echo "BUILD_DIR=${BUILD_DIR}"

docker run $@ \
  --rm \
  -e DEBUG=${DEBUG:-false} \
  -e PGLITE_VERSION=${PGLITE_VERSION} \
  ${BUILD_DIR_ARGS} \
  --workdir=${DOCKER_WORKSPACE} \
  -v .:${DOCKER_WORKSPACE}:rw \
  -v ./dist:/pglite:rw \
  electricsql/pglite-builder:6.0.13-1  \
  ./build-pglite.sh
