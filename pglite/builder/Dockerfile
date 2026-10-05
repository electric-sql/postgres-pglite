ARG EMSDK_VER=3.1.74
ARG BUILDPLATFORM
# we only need to build on one platform, since we're interested in the WASM output
# building on the native (BUILDPLATFORM) is much faster, so use that
# remove "-arm64" suffix if building on x86_64
FROM --platform=$BUILDPLATFORM emscripten/emsdk:${EMSDK_VER} AS builder

ENV LLVM_NM=/emsdk/upstream/bin/llvm-nm

RUN apt update && apt upgrade -y && apt install -y \
    xz-utils autoconf libtool automake pkgconf bison flex

SHELL ["/bin/bash", "-c"]

RUN mkdir -p /install/libs /install/exports /install/imports

WORKDIR /install/libs

# zlib CAN be installed by using the emsdk like this
# RUN embuilder --pic --verbose build zlib

WORKDIR /src

# ENV EMCC_COMMON_FLAGS="-fPIC -sWASM_BIGINT -sMIN_SAFARI_VERSION=150000 -O2 -m32 -D_FILE_OFFSET_BITS=64 -sSUPPORT_LONGJMP=emscripten -mno-bulk-memory -mnontrapping-fptoint -mno-reference-types -mno-sign-ext -mno-extended-const -mno-atomics -mno-tail-call -mno-multivalue -mno-relaxed-simd -mno-simd128 -mno-multimemory -mno-exception-handling -Wno-unused-command-line-argument -Wno-unreachable-code-fallthrough -Wno-unused-function -Wno-invalid-noreturn -Wno-declaration-after-statement -Wno-invalid-noreturn"
ENV EMCC_COMMON_FLAGS="-O2 -fPIC -sWASM_BIGINT"

WORKDIR /src
RUN curl -L https://www.zlib.net/zlib-1.3.1.tar.gz | tar -xz
WORKDIR /src/zlib-1.3.1
RUN CFLAGS="${EMCC_COMMON_FLAGS}" CXXFLAGS="${EMCC_COMMON_FLAGS}" emconfigure ./configure --static --prefix=/install/libs
RUN emmake make -j && emmake make install
RUN ${LLVM_NM} /install/libs/lib/libz.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libz.exports

WORKDIR /src
RUN curl -L https://gitlab.gnome.org/GNOME/libxml2/-/archive/v2.14.5/libxml2-v2.14.5.tar.gz | tar -xz
WORKDIR /src/libxml2-v2.14.5
RUN ./autogen.sh --with-python=no --with-threads=no
RUN CFLAGS="${EMCC_COMMON_FLAGS}" CXXFLAGS="${EMCC_COMMON_FLAGS}" emconfigure ./configure --enable-shared=no --enable-static=yes --with-python=no --prefix=/install/libs
RUN emmake make -j && emmake make install
# extract exported symbols - useful for passing them to emscripten as EXPORTED_FUNCTIONS 
RUN ${LLVM_NM} /install/libs/lib/libxml2.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libxml2.exports

WORKDIR /src
RUN curl -L https://gitlab.gnome.org/GNOME/libxslt/-/archive/v1.1.43/libxslt-v1.1.43.tar.gz | tar -xz
WORKDIR /src/libxslt-v1.1.43
RUN ./autogen.sh --with-python=no
RUN CFLAGS="${EMCC_COMMON_FLAGS}" CXXFLAGS="${EMCC_COMMON_FLAGS}" emconfigure ./configure --enable-shared=no --enable-static=yes --with-python=no --prefix=/install/libs --with-libxml-src=/src/libxml2-v2.14.5/ --with-pic=yes
RUN emmake make -j && emmake make install
# extract exported symbols - useful for passing them to emscripten as EXPORTED_FUNCTIONS 
RUN ${LLVM_NM} /install/libs/lib/libxslt.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libxslt.exports

WORKDIR /src
RUN curl -L https://github.com/openssl/openssl/releases/download/openssl-3.0.17/openssl-3.0.17.tar.gz | tar xz
WORKDIR /src/openssl-3.0.17
RUN CFLAGS="${EMCC_COMMON_FLAGS}" CXXFLAGS="${EMCC_COMMON_FLAGS}" emconfigure ./config no-sse2 no-hw no-asm no-tests no-threads no-shared linux-generic32 --prefix=/install/libs
RUN sed -i 's|^CROSS_COMPILE.*$|CROSS_COMPILE=|g' Makefile # see https://github.com/emscripten-core/emscripten/issues/19597#issue-1754476454
RUN emmake make -j && emmake make install_sw install_ssldirs
# extract exported symbols - useful for passing them to emscripten as EXPORTED_FUNCTIONS 
RUN ${LLVM_NM} /install/libs/lib/libssl.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libssl.exports
RUN ${LLVM_NM} /install/libs/lib/libcrypto.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libcrypto.exports

WORKDIR /src
RUN curl -L ftp://ftp.ossp.org/pkg/lib/uuid/uuid-1.6.2.tar.gz | tar xz
# COPY . .
# RUN tar xvf uuid-1.6.2.tar.gz
WORKDIR /src/uuid-1.6.2
RUN CFLAGS="${EMCC_COMMON_FLAGS}" CXXFLAGS="${EMCC_COMMON_FLAGS}" \
emconfigure ./configure \
  ac_cv_header_ifaddrs_h=no \
  ac_cv_header_net_if_h=no \
  ac_cv_header_net_if_dl_h=no \
  ac_cv_header_net_if_arp_h=no \
  ac_cv_header_sys_ioctl_h=no \
  ac_cv_func_getifaddrs=no \
  --build=wasm32-unknown-linux-gnu --enable-shared=no --enable-static=yes --with-perl=no --with-perl-compat=no --prefix=/install/libs --with-php=no --with-pic=yes
RUN emmake make -j && emmake make install || true # install tries to strip the wasm, but it doesnt recognize the format, so ignore atm
WORKDIR /install/libs/lib
RUN ln -s libuuid.a libossp-uuid.a # contrib extensions use -lossp-uuid
RUN ${LLVM_NM} /install/libs/lib/libossp-uuid.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libossp-uuid.exports

# Build ICU from source
WORKDIR /src
RUN curl -L https://github.com/unicode-org/icu/releases/download/release-76-1/icu4c-76_1-src.tgz | tar -xz

# Step 1: Build native ICU (needed for cross-compilation tools like icupkg, gencnval)
WORKDIR /src/icu/source
RUN mkdir /src/icu-native
RUN cd /src/icu-native && \
    /src/icu/source/configure --disable-shared --enable-static --disable-tests --disable-samples && \
    make -j

RUN mkdir -p /install/libs/share/icu/76.1/icudt76l/brkitr /install/libs/share/icu/76.1/icudt76l/coll /install/libs/share/icu/76.1/icudt76l/curr /install/libs/share/icu/76.1/icudt76l/lang /install/libs/share/icu/76.1/icudt76l/rbnf /install/libs/share/icu/76.1/icudt76l/region /install/libs/share/icu/76.1/icudt76l/translit /install/libs/share/icu/76.1/icudt76l/unit /install/libs/share/icu/76.1/icudt76l/zone

# Step 2: Build wasm ICU
# --with-data-packaging: https://unicode-org.github.io/icu/userguide/icu_data/#building-and-linking-against-icu-data
WORKDIR /src/icu-wasm
RUN CFLAGS="${EMCC_COMMON_FLAGS}" CXXFLAGS="${EMCC_COMMON_FLAGS} -std=c++17" \
    emconfigure /src/icu/source/configure \
    --host=wasm32-unknown-linux-gnu \
    --with-cross-build=/src/icu-native \
    --with-data-packaging=archive \
    --disable-shared \
    --enable-static \
    --disable-tests \
    --disable-samples \
    --disable-extras \
    --disable-icuio \
    --disable-layoutex \
    --prefix=/install/libs
RUN emmake make -j PKGDATA_OPTS="--without-assembly -O $(pwd)/data/icupkg.inc"
RUN emmake make install

# for postgis
WORKDIR /src
RUN git clone --depth 1 --single-branch --branch=json-c-0.18-20240915 https://github.com/json-c/json-c.git
WORKDIR /src/json-c/build
RUN emcmake cmake \
-DCMAKE_BUILD_TYPE=Release \
-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
-DCMAKE_INSTALL_PREFIX=/install/libs \
-DENABLE_THREADING=OFF -DBUILD_SHARED_LIBS=OFF -DDISABLE_EXTRA_LIBS=ON -DBUILD_APPS=OFF -DBUILD_TESTING=OFF ..
RUN cmake --build . -j && cmake --build . --target install
RUN ${LLVM_NM} /install/libs/lib/libjson-c.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libjson-c.exports

WORKDIR /src
RUN git clone --depth 1 --single-branch --branch=v1.24 https://github.com/ebiggers/libdeflate.git
WORKDIR /src/libdeflate/build
RUN emcmake cmake \
-DCMAKE_BUILD_TYPE=Release \
-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
-DCMAKE_INSTALL_PREFIX=/install/libs \
-DLIBDEFLATE_BUILD_SHARED_LIB=OFF -DLIBDEFLATE_BUILD_TESTS=OFF ..
RUN cmake --build . -j && cmake --build . --target install
RUN ${LLVM_NM} /install/libs/lib/libdeflate.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libdeflate.exports

WORKDIR /src
RUN curl -L https://gitlab.com/libtiff/libtiff/-/archive/v4.7.1/libtiff-v4.7.1.tar.gz | tar xz
# RUN tar -xvf libtiff-v4.7.1.tar.gz
WORKDIR /src/libtiff-v4.7.1
RUN ./autogen.sh
# we're disabling jpeg here but gdal uses jpeg (internal build if not specified - so it would make sense to enable it here as well)
RUN emconfigure ./configure \
--with-pic --disable-webp --disable-zstd --disable-lzma --disable-jbig --disable-old-jpeg \
--disable-jpeg --disable-pixarlog --disable-mdi --disable-opengl --disable-win32-io \
--with-zlib-include-dir=/install/libs/include --with-zlib-lib-dir=/install/libs/lib \
--with-libdeflate-include-dir=/install/libs/include --with-libdeflate-lib-dir=/install/libs/lib/ \
--prefix=/install/libs --enable-shared=no
RUN emmake make -j && emmake make install
RUN ${LLVM_NM} /install/libs/lib/libtiff.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libtiff.exports

WORKDIR /src
RUN wget https://www.sqlite.org/2025/sqlite-src-3500400.zip 
RUN unzip sqlite-src-3500400.zip
# RUN unzip sqlite-src-3500400.zip
WORKDIR /src/sqlite-src-3500400
RUN emconfigure ./configure --disable-shared --disable-threadsafe --prefix=/install/libs --with-emsdk
RUN emmake make -j && emmake make install 
# we need a sqlite3 executable for the system on which we're building (needed by https://proj.org/en/stable/install.html#build-requirements)
RUN emmake make clean && ./configure --disable-shared && make -j && cp sqlite3 /tmp/sqlite3
RUN ${LLVM_NM} /install/libs/lib/libsqlite3.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libsqlite3.exports

WORKDIR /src
RUN wget https://download.osgeo.org/proj/proj-9.7.0.tar.gz
RUN tar -xvf proj-9.7.0.tar.gz
WORKDIR /src/proj-9.7.0
RUN emcmake cmake \
-DCMAKE_BUILD_TYPE=Release \
-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
-DCMAKE_INSTALL_PREFIX:PATH=/install/libs \
-DEMBED_RESOURCE_FILES=OFF \
-DBUILD_SHARED_LIBS=OFF \
-DSQLite3_INCLUDE_DIR=/install/libs/include/ -DSQLite3_LIBRARY=/install/libs/lib/libsqlite3.a -DEXE_SQLITE3=/tmp/sqlite3 \
-DTIFF_LIBRARY=/install/libs/lib/libtiff.a -DTIFF_INCLUDE_DIR=/install/libs/include/ \
-DENABLE_CURL=OFF -DBUILD_APPS=OFF -DBUILD_TESTING=OFF
RUN cmake --build . -j && cmake --build . --target install
RUN ${LLVM_NM} /install/libs/lib/libproj.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libproj.exports

WORKDIR /src
RUN wget https://download.osgeo.org/geos/geos-3.14.0.tar.bz2
RUN tar -xvf geos-3.14.0.tar.bz2
WORKDIR /src/geos-3.14.0/build
RUN emcmake cmake \
-DCMAKE_BUILD_TYPE=Release \
-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
-DCMAKE_INSTALL_PREFIX=/install/libs \
-DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_GEOSOP=OFF ..
RUN emmake make -j && emmake make install
RUN ${LLVM_NM} /install/libs/lib/libgeos.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libgeos.exports

WORKDIR /src
RUN wget https://github.com/OSGeo/gdal/releases/download/v3.11.4/gdal-3.11.4.tar.gz
RUN tar -xvf gdal-3.11.4.tar.gz
WORKDIR /src/gdal-3.11.4/build
# Docs: https://gdal.org/en/stable/development/building_from_source.html
RUN emcmake cmake \
-DCMAKE_C_FLAGS="-fPIC" \
-DCMAKE_CXX_FLAGS="-fPIC" \
-DCMAKE_BUILD_TYPE=Release \
-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
-DCMAKE_INSTALL_PREFIX=/install/libs \
-DLIBXML2_INCLUDE_DIR=/install/libs/include/libxml2 -DLIBXML2_LIBRARY=/install/libs/lib/libxml2.a \
-DGEOS_INCLUDE_DIR=/install/libs/include/geos -DGEOS_LIBRARY=/install/libs/lib/libgeos.a \
-DTIFF_INCLUDE_DIR=/install/libs/include/ -DTIFF_LIBRARY=/install/libs/lib/libtiff.a \
-DSQLite3_INCLUDE_DIR=/install/libs/include/ -DSQLite3_LIBRARY=/install/libs/lib/libsqlite3.a -DEXE_SQLITE3=/tmp/sqlite3 \
-DZLIB_LIBRARY=/install/libs/lib/libz.a -DZLIB_INCLUDE_DIR=/install/libs/include \
-DOPENSSL_ROOT_DIR=/install/libs \
-DPROJ_LIBRARY=/install/libs/lib/libproj.a -DPROJ_INCLUDE_DIR=/install/libs/include/ \
-DJSONC_LIBRARY=/install/libs/lib/libjson-c.a -DJSONC_INCLUDE_DIR=/install/libs/include/json-c -DGDAL_USE_JSONC=ON -DGDAL_USE_JSONC_INTERNAL=OFF \
-DBUILD_SHARED_LIBS=OFF -DBUILD_APPS=OFF -DGDAL_BUILD_OPTIONAL_DRIVERS=OFF -DOGR_BUILD_OPTIONAL_DRIVERS=OFF \
-DGDAL_USE_HDFS=OFF \
-DACCEPT_MISSING_SQLITE3_MUTEX_ALLOC:BOOL=ON \
..
RUN cmake --build . -j && cmake --build . --target install
RUN ${LLVM_NM} /install/libs/lib/libgdal.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libgdal.exports
RUN find . -type f \( -name "*.o" -o -name "*.so" \) -exec ${LLVM_NM} --defined-only {} \; | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /tmp/libgdal.defs.txt
RUN find . -name "*.o" -exec ${LLVM_NM} --undefined-only {} \; | awk '{print $2}' | sed '/^$/d' | sort -u > /tmp/libgdal.undef.txt
RUN comm -23 /tmp/libgdal.undef.txt /tmp/libgdal.defs.txt > /install/imports/libgdal.imports

ARG TARGETARCH
FROM emscripten/emsdk:${EMSDK_VER} AS base-amd64
FROM emscripten/emsdk:${EMSDK_VER}-arm64 AS base-arm64
FROM base-${TARGETARCH} AS runner

SHELL ["/bin/bash", "-c"]

RUN apt update && apt upgrade -y && apt install -y \
    xz-utils autoconf libtool automake pkgconf bison flex

# this is where the libraries will be installed and subsequently where the LIBS and INCLUDES can be found
ARG INSTALL_PREFIX=/install/libs
ENV INSTALL_PREFIX=${INSTALL_PREFIX}

ARG LIB_EXPORTS_DIR=/install/exports
ENV LIB_EXPORTS_DIR=${LIB_EXPORTS_DIR}

COPY --from=builder /install/libs ${INSTALL_PREFIX}
COPY --from=builder /install/exports ${LIB_EXPORTS_DIR}

# allow access to anyone 
RUN chmod -R 777 /install

# needed in building pglite.wasm
ENV LLVM_NM=/emsdk/upstream/bin/llvm-nm