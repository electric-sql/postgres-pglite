# although we could use any path inside docker, using the same path as on the host
# allows the DWARF info (when building in DEBUG) to contain the correct file paths
DOCKER_WORKSPACE=$(pwd)
echo "PGLITE_VERSION=${PGLITE_VERSION}"
echo "DEBUG=${DEBUG}"

docker run $@ \
  --rm \
  -e DEBUG=${DEBUG:-false} \
  -e PGLITE_VERSION=${PGLITE_VERSION} \
  -e PGLITE_BUILD_SHARED_MEMORY=${PGLITE_BUILD_SHARED_MEMORY:-false} \
  -e PGLITE_SHARED_MEMORY_SIZE=${PGLITE_SHARED_MEMORY_SIZE:-256MB} \
  -e PGLITE_INITIAL_MEMORY_SIZE=${PGLITE_INITIAL_MEMORY_SIZE:-128MB} \
  -e PGLITE_FORCE_CLEAN=${PGLITE_FORCE_CLEAN:-false} \
  -e PGLITE_MAKE_JOBS=${PGLITE_MAKE_JOBS:-} \
  --workdir=${DOCKER_WORKSPACE} \
  -v .:${DOCKER_WORKSPACE}:rw \
  -v ./dist:/pglite:rw \
  electricsql/pglite-builder:3.1.74-7 \
  ./build-pglite.sh
