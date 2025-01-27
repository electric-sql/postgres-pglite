#!/bin/bash
echo "======== build-all.sh : $(pwd)             =========="
echo "======== Building all PGlite prerequisites =========="

. ./cibuild.sh contrib extra node linkweb postgres-pglite-dist
