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

IMG_NAME="electricsql/pglite-builder"
IMG_TAG="${PG_VERSION}_${SDK_VERSION}"
SDK_ARCHIVE="${SDK_ARCHIVE:-python3.13-wasm-sdk-Ubuntu-22.04.tar.lz4}"
WASI_SDK_ARCHIVE="${WASI_SDK_ARCHIVE:-python3.13-wasi-sdk-Ubuntu-22.04.tar.lz4}"

#!/bin/bash
echo "======== build-all.sh : $(pwd)             =========="
echo "======== Building all PGlite prerequisites =========="

# move copy of patches into dir
# not mounting them directly as lots of files are created
# cp -rf /opt/patches ./patches

apt update && apt install -y build-essential libreadline-dev zlib1g-dev bison flex git
export FLEX=`which flex`

pushd pglite

. ./cibuild.sh

. ./cibuild.sh contrib
. ./cibuild.sh extra
. ./cibuild.sh node
. ./cibuild.sh linkweb
. ./cibuild.sh postgres-pglite-dist

popd