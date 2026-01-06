# although we could use any path inside docker, using the same path as on the host
# allows the DWARF info (when building in DEBUG) to contain the correct file paths
DOCKER_WORKSPACE=$(pwd)
GITHUB_ACTIONS=${GITHUB_ACTIONS:-"false"}
if [ "$GITHUB_ACTIONS" = "true" ]; then
    echo "Running inside GitHub Actions"
else
    echo "Not running inside GitHub Actions"
fi

docker run $@ \
  --rm \
  -e DEBUG=${DEBUG:-false} \
  --workdir=${DOCKER_WORKSPACE} \
  -v .:${DOCKER_WORKSPACE}:rw \
  -v ./dist:/install/pglite:rw \
  electricsql/pglite-builder:3.1.74-postgis_4 \
  ./build-pglite.sh
  
