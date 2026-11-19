#!/bin/bash
set -euo pipefail
echo "=== Building postgis ==="
POSTGIS_PROFILE_FILE=../../../.pglite-postgis-build-profile
POSTGIS_LDFLAGS_SL=${PGLITE_LDFLAGS_SL//-shared/}
POSTGIS_PROFILE=$(printf '%s\n' \
  "emcc_cflags=${EMCC_CFLAGS:-}" \
  "cflags=${PGLITE_CFLAGS}" \
  "ldflags=${POSTGIS_LDFLAGS_SL}")
POSTGIS_RECONFIGURE=false
pushd other_extensions/postgis
# hack - building loader/pgsql2shp.wasm fails, we don't need it anyway
sed -i 's/SUBDIRS += @RASTER@ loader/SUBDIRS += @RASTER@/' GNUmakefile.in
# hack - building raster loader fails, but anyway, we don't need it
sed -i 's/$(MAKE) @RT_LOADER@/#$(MAKE) @RT_LOADER@/' ./raster/Makefile.in
sed -i 's/$(MAKE) -C loader install/#$(MAKE) -C loader install/' ./raster/Makefile.in
if [ ! -f "${POSTGIS_PROFILE_FILE}" ] ||
   [ "$(cat "${POSTGIS_PROFILE_FILE}" 2>/dev/null || true)" != "${POSTGIS_PROFILE}" ] ||
   [ ! -f Makefile ]; then
  POSTGIS_RECONFIGURE=true
fi
if [ "${POSTGIS_RECONFIGURE}" = true ]; then
./autogen.sh
# LDFLAGS="-L/install/libs/lib" emconfigure ./configure --with-pic --with-xml2config=/install/libs/bin/xml2-config --with-geosconfig=/install/libs/bin/geos-config --with-projdir=/install/libs/ --with-jsondir=/install/libs/ --without-protobuf --with-gdalconfig=/install/libs/bin/gdal-config
# emconfigure ./configure --with-pic --with-geosconfig=/install/libs/bin/geos-config --with-projdir=/install/libs/ --with-xml2config=/install/libs/bin/xml2-config --with-jsondir=/install/libs/ --without-protobuf --without-raster --enable-static=no --enable-shared=yes
PROJ_VERSION=9.7.0 \
ac_cv_file__install_libs_include_json_c_json_h=yes \
LDFLAGS="-L/install/libs/lib $PGLITE_CFLAGS" \
CFLAGS="${PGLITE_CFLAGS} -fPIC" \
CXXFLAGS="${PGLITE_CFLAGS} -fPIC" \
emconfigure ./configure \
--with-pic \
--without-protobuf \
--enable-static=no \
--enable-shared=yes \
--with-geosconfig=/install/libs/bin/geos-config \
--with-xml2config=/install/libs/bin/xml2-config \
--with-gdalconfig=/install/libs/bin/gdal-config \
--with-projdir=/install/libs \
--with-jsondir=/install/libs \
--host=wasm32-unknown-linux-gnu
emmake make clean >/dev/null 2>&1 || true
# PostGIS's top-level clean does not descend into this bundled C++ archive.
# Its objects are target-specific and must not cross from classic memory into
# a shared-memory side module.
emmake make -C deps/flatgeobuf clean >/dev/null 2>&1 || true
else
  echo "postgis: reusing objects for the effective PGlite build profile."
fi
emmake make raster-sql || true
# these flags are used in pgxs.mk (postgresql extension makefile) and passed to the build process of that extension
find . -name '*.so' -delete
# Preserve the compiler ABI flags and bundle libc++ explicitly: Emscripten
# ignores -lstdc++. Link libc++abi after the whole-archive group for C++ guards.
POSTGIS_COMMON_LDFLAGS="$PGLITE_CFLAGS $POSTGIS_LDFLAGS_SL -Wl,--whole-archive -lc++ -lsqlite3 -lgeos -ljson-c"
POSTGIS_MAKE_FLAGS=(
  "CFLAGS_SL=-sWASM_BIGINT $PGLITE_CFLAGS"
  "CXXFLAGS_SL=-sWASM_BIGINT $PGLITE_CFLAGS"
  -j1
)
emmake make LIBGDAL_LDFLAGS= \
  LDFLAGS_SL="$POSTGIS_COMMON_LDFLAGS -Wl,--no-whole-archive -lc++abi" \
  "${POSTGIS_MAKE_FLAGS[@]}" || { echo 'emmake make postgis failed' ; exit 442; }

# All raster datasets must use one GDAL copy, including its shared-dataset
# registry. Mixing copies leaves stale handles after a dataset is closed.
find postgis -maxdepth 1 -type f -name '*.so' -delete
emmake make -C postgis \
  LDFLAGS_SL="$POSTGIS_COMMON_LDFLAGS -lgdal -ltiff -lz -ldeflate -Wl,--no-whole-archive -lc++abi" \
  "${POSTGIS_MAKE_FLAGS[@]}" || { echo 'emmake make PostGIS GDAL provider failed' ; exit 442; }
printf '%s\n' "${POSTGIS_PROFILE}" >"${POSTGIS_PROFILE_FILE}"
# emmake make PG_LDFLAGS="-L/install/libs/lib -lpgport -lpgcommon -sSIDE_MODULE=1" -j

popd
