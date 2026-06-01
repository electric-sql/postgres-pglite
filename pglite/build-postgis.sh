#!/bin/bash
echo "=== Building postgis ==="
pushd other_extensions/postgis
# hack - building loader/pgsql2shp.wasm fails, we don't need it anyway
sed -i 's/SUBDIRS += @RASTER@ loader/SUBDIRS += @RASTER@/' GNUmakefile.in
# hack - building raster loader fails, but anyway, we don't need it
sed -i 's/$(MAKE) @RT_LOADER@/#$(MAKE) @RT_LOADER@/' ./raster/Makefile.in
sed -i 's/$(MAKE) -C loader install/#$(MAKE) -C loader install/' ./raster/Makefile.in
./autogen.sh
# LDFLAGS="-L/install/libs/lib" emconfigure ./configure --with-pic --with-xml2config=/install/libs/bin/xml2-config --with-geosconfig=/install/libs/bin/geos-config --with-projdir=/install/libs/ --with-jsondir=/install/libs/ --without-protobuf --with-gdalconfig=/install/libs/bin/gdal-config
# emconfigure ./configure --with-pic --with-geosconfig=/install/libs/bin/geos-config --with-projdir=/install/libs/ --with-xml2config=/install/libs/bin/xml2-config --with-jsondir=/install/libs/ --without-protobuf --without-raster --enable-static=no --enable-shared=yes
PROJ_VERSION=9.7.0 LDFLAGS="-L/install/libs/lib $PGLITE_CFLAGS" CFLAGS="${PGLITE_CFLAGS} -fPIC" CXXFLAGS="${PGLITE_CFLAGS} -fPIC" emconfigure ./configure \
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
emmake make raster-sql || true
# these flags are used in pgxs.mk (postgresql extension makefile) and passed to the build process of that extension
emmake make LDFLAGS_SL="$PGLITE_CFLAGS-sWASM_BIGINT -sSIDE_MODULE=1 -Wl,--whole-archive -lstdc++ -lsqlite3 -lgeos -ljson-c -lgdal -Wl,--no-whole-archive" \
CFLAGS_SL="-sWASM_BIGINT $PGLITE_CFLAGS" \
CXXFLAGS_SL="-sWASM_BIGINT $PGLITE_CFLAGS" -j1 || { echo 'emmake make postgis failed' ; exit 442; }
# emmake make PG_LDFLAGS="-L/install/libs/lib -lpgport -lpgcommon -sSIDE_MODULE=1" -j

popd