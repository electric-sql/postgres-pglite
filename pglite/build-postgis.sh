#!/bin/sh
echo "=== Building postgis ==="
cd postgis
# hack - building loader/pgsql2shp.wasm fails, we don't need it anyway
sed -i 's/SUBDIRS += @RASTER@ loader/SUBDIRS += @RASTER@/' GNUmakefile.in
./autogen.sh
# LDFLAGS="-L/install/libs/lib" emconfigure ./configure --with-pic --with-xml2config=/install/libs/bin/xml2-config --with-geosconfig=/install/libs/bin/geos-config --with-projdir=/install/libs/ --with-jsondir=/install/libs/ --without-protobuf --with-gdalconfig=/install/libs/bin/gdal-config
# emconfigure ./configure --with-pic --with-geosconfig=/install/libs/bin/geos-config --with-projdir=/install/libs/ --with-xml2config=/install/libs/bin/xml2-config --with-jsondir=/install/libs/ --without-protobuf --without-raster --enable-static=no --enable-shared=yes
PROJ_VERSION=9.7.0 LDFLAGS="-L/install/libs/lib" CFLAGS="${PGLITE_CFLAGS}" CXXFLAGS="${PGLITE_CFLAGS}" emconfigure ./configure \
--with-geosconfig=/install/libs/bin/geos-config \
--with-pic --without-protobuf --without-raster --enable-static=no --enable-shared=yes --with-xml2config=/install/libs/bin/xml2-config --with-projdir=/install/libs/ --host=wasm32-unknown-none
# touch ./loader/pgsql2shp.wasm
emmake make raster-sql || true
# these flags are used in pgxs.mk (postgresql extension makefile) and passed to the build process of that extension
emmake make LDFLAGS_SL="-sWASM_BIGINT -sSIDE_MODULE=1 -fexceptions -Wl,--whole-archive -lstdc++ -lsqlite3 -lgeos -Wl,--no-whole-archive" \
CFLAGS_SL="-fexceptions -sWASM_BIGINT" \
CXXFLAGS_SL="-fexceptions -sWASM_BIGINT" -j || { echo 'emmake make postgis failed' ; exit 442; }
# emmake make PG_LDFLAGS="-L/install/libs/lib -lpgport -lpgcommon -sSIDE_MODULE=1" -j

cd ..
