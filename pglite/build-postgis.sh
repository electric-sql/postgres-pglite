#!/bin/sh
cd postgis
./autogen.sh
# LDFLAGS="-L/install/libs/lib" emconfigure ./configure --with-pic --with-xml2config=/install/libs/bin/xml2-config --with-geosconfig=/install/libs/bin/geos-config --with-projdir=/install/libs/ --with-jsondir=/install/libs/ --without-protobuf --with-gdalconfig=/install/libs/bin/gdal-config
# emconfigure ./configure --with-pic --with-geosconfig=/install/libs/bin/geos-config --with-projdir=/install/libs/ --with-xml2config=/install/libs/bin/xml2-config --with-jsondir=/install/libs/ --without-protobuf --without-raster --enable-static=no --enable-shared=yes
PROJ_VERSION=9.7.0 LDFLAGS="-L/install/libs/lib" emconfigure ./configure --with-pic --without-protobuf --without-raster --enable-static=no --enable-shared=yes --with-xml2config=/install/libs/bin/xml2-config --with-geosconfig=/install/libs/bin/geos-config --with-projdir=/install/libs/ --host=wasm32-unknown-none
emmake make LDFLAGS_SL="-sWASM_BIGINT -L/install/libs/lib -sSIDE_MODULE=1 -fexceptions -Wl,--whole-archive -lstdc++ -Wl,--no-whole-archive" CFLAGS_SL="-fexceptions -sWASM_BIGINT" CXXFLAGS_SL="-fexceptions -sWASM_BIGINT" -j
# emmake make PG_LDFLAGS="-L/install/libs/lib -lpgport -lpgcommon -sSIDE_MODULE=1" -j

cd ..
