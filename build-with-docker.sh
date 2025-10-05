# although we could use any path inside docker, using the same path as on the host
# allows the DWARF info (when building in DEBUG) to contain the correct file paths
DOCKER_WORKSPACE=$(pwd)
GITHUB_ACTIONS=${GITHUB_ACTIONS:-"false"}
ADDITIONAL_DOCKER_PARAMS=""
if [ "$GITHUB_ACTIONS" = "true" ]; then
    echo "Running inside GitHub Actions"
    ADDITIONAL_DOCKER_PARAMS=""
else
    echo "Not running inside GitHub Actions"
    ADDITIONAL_DOCKER_PARAMS="-it -u $(id -u):$(id -g)"
fi

docker run $@ \
  $ADDITIONAL_DOCKER_PARAMS \
  --rm \
  -e DEBUG=${DEBUG:-false} \
  --workdir=${DOCKER_WORKSPACE} \
  -v .:${DOCKER_WORKSPACE}:rw \
  -v ./dist:/install/pglite:rw \
  electricsql/pglite-builder:3.1.74-postgis \
  ./build-pglite.sh
  
