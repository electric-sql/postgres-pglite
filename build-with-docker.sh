# although we could use any path inside docker, using the same path as on the host
# allows the DWARF info (when building in DEBUG) to contain the correct file paths
DOCKER_WORKSPACE=$(pwd)
echo "PGLITE_VERSION=${PGLITE_VERSION}"
echo "DEBUG=${DEBUG}"

docker run $@ \
  --rm \
  -e DEBUG=${DEBUG:-false} \
  -e PGLITE_VERSION=${PGLITE_VERSION} \
  --workdir=${DOCKER_WORKSPACE} \
  -v .:${DOCKER_WORKSPACE}:rw \
  -v ./dist:/pglite:rw \
  electricsql/pglite-builder:3.1.74-7 \
  ./build-pglite.sh
