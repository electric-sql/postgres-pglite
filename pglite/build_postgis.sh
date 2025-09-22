#!/bin/sh
cd postgis
./autogen.sh
LDFLAGS="-L/install/libs/lib" emconfigure ./configure --with-pic --with-xml2config=/install/libs/bin/xml2-config --with-geosconfig=/install/libs/bin/geos-config --with-projdir=/install/libs/ --with-jsondir=/install/libs/ --without-protobuf --with-gdalconfig=/install/libs/bin/gdal-config
cd ..
