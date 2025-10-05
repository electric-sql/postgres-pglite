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

RUN mkdir -p /install/libs /install/exports

WORKDIR /install/libs

# zlib CAN be installed by using the emsdk like this
# RUN embuilder --pic --verbose build zlib

WORKDIR /src

# ENV EMCC_COMMON_FLAGS="-fPIC -sWASM_BIGINT -sMIN_SAFARI_VERSION=150000 -O2 -m32 -D_FILE_OFFSET_BITS=64 -sSUPPORT_LONGJMP=emscripten -mno-bulk-memory -mnontrapping-fptoint -mno-reference-types -mno-sign-ext -mno-extended-const -mno-atomics -mno-tail-call -mno-multivalue -mno-relaxed-simd -mno-simd128 -mno-multimemory -mno-exception-handling -Wno-unused-command-line-argument -Wno-unreachable-code-fallthrough -Wno-unused-function -Wno-invalid-noreturn -Wno-declaration-after-statement -Wno-invalid-noreturn"
ENV EMCC_COMMON_FLAGS="-O2 -fPIC"

COPY ./prebuilts/ /src/

WORKDIR /src
# RUN curl -L https://www.zlib.net/zlib-1.3.1.tar.gz | tar -xz
RUN tar -xzf zlib-1.3.1.tar.gz
WORKDIR /src/zlib-1.3.1
RUN CFLAGS="${EMCC_COMMON_FLAGS}" CXXFLAGS="${EMCC_COMMON_FLAGS}" emconfigure ./configure --static --prefix=/install/libs
RUN emmake make -j && emmake make install
RUN ${LLVM_NM} /install/libs/lib/libz.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libz.exports

WORKDIR /src
# RUN curl -L https://gitlab.gnome.org/GNOME/libxml2/-/archive/v2.14.5/libxml2-v2.14.5.tar.gz | tar -xz
RUN tar -xvf libxml2-v2.14.5.tar.gz
WORKDIR /src/libxml2-v2.14.5
RUN ./autogen.sh --with-python=no
RUN CFLAGS="${EMCC_COMMON_FLAGS}" CXXFLAGS="${EMCC_COMMON_FLAGS}" emconfigure ./configure --enable-shared=no --enable-static=yes --with-python=no --prefix=/install/libs
RUN emmake make -j && emmake make install
# extract exported symbols - useful for passing them to emscripten as EXPORTED_FUNCTIONS 
RUN ${LLVM_NM} /install/libs/lib/libxml2.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libxml2.exports

WORKDIR /src
# RUN curl -L https://gitlab.gnome.org/GNOME/libxslt/-/archive/v1.1.43/libxslt-v1.1.43.tar.gz | tar -xz
RUN tar -xvf libxslt-v1.1.43.tar.gz
WORKDIR /src/libxslt-v1.1.43
RUN ./autogen.sh --with-python=no
RUN CFLAGS="${EMCC_COMMON_FLAGS}" CXXFLAGS="${EMCC_COMMON_FLAGS}" emconfigure ./configure --enable-shared=no --enable-static=yes --with-python=no --prefix=/install/libs --with-libxml-src=/src/libxml2-v2.14.5/ --with-pic=yes
RUN emmake make -j && emmake make install
# extract exported symbols - useful for passing them to emscripten as EXPORTED_FUNCTIONS 
RUN ${LLVM_NM} /install/libs/lib/libxslt.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libxslt.exports

WORKDIR /src
# RUN curl -L https://github.com/openssl/openssl/releases/download/openssl-3.0.17/openssl-3.0.17.tar.gz | tar xz
RUN tar -xvf openssl-3.0.17.tar.gz
WORKDIR /src/openssl-3.0.17
RUN emconfigure ./Configure no-tests linux-generic64 --prefix=/install/libs
RUN sed -i 's|^CROSS_COMPILE.*$|CROSS_COMPILE=|g' Makefile # see https://github.com/emscripten-core/emscripten/issues/19597#issue-1754476454
RUN emmake make -j && emmake make install
# extract exported symbols - useful for passing them to emscripten as EXPORTED_FUNCTIONS 
RUN ${LLVM_NM} /install/libs/lib/libssl.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libssl.exports

WORKDIR /src
RUN curl -L ftp://ftp.ossp.org/pkg/lib/uuid/uuid-1.6.2.tar.gz | tar xz
# COPY . .
# RUN tar xvf uuid-1.6.2.tar.gz
WORKDIR /src/uuid-1.6.2
RUN CFLAGS="${EMCC_COMMON_FLAGS}" CXXFLAGS="${EMCC_COMMON_FLAGS}" emconfigure ./configure --build=aarch64-unknown-linux-gnu --enable-shared=no --enable-static=yes --with-perl=no --with-perl-compat=no --prefix=/install/libs --with-php=no --with-pic=yes
RUN emmake make -j && emmake make install || true # install tries to strip the wasm, but it doesnt recognize the format, so ignore atm
WORKDIR /install/libs/lib
RUN ln -s libuuid.a libossp-uuid.a # contrib extensions use -lossp-uuid
RUN ${LLVM_NM} /install/libs/lib/libossp-uuid.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libossp-uuid.exports

WORKDIR /src
RUN git clone --depth 1 --single-branch --branch=json-c-0.18-20240915 https://github.com/json-c/json-c.git
WORKDIR /src/json-c/build
RUN emcmake cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_BUILD_TYPE=release -DENABLE_THREADING=OFF -DBUILD_SHARED_LIBS=OFF -DDISABLE_EXTRA_LIBS=ON -DCMAKE_INSTALL_PREFIX=/install/libs ..
RUN cmake --build . -j && cmake --build . --target install
RUN ${LLVM_NM} /install/libs/lib/libjson-c.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libjson-c.exports

WORKDIR /src
RUN git clone --depth 1 --single-branch --branch=v1.24 https://github.com/ebiggers/libdeflate.git
WORKDIR /src/libdeflate/build
RUN emcmake cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=/install/libs -DLIBDEFLATE_BUILD_SHARED_LIB=OFF -DLIBDEFLATE_BUILD_TESTS=OFF ..
RUN cmake --build . -j && cmake --build . --target install
RUN ${LLVM_NM} /install/libs/lib/libdeflate.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libdeflate.exports

WORKDIR /src
# RUN curl -L https://gitlab.com/libtiff/libtiff/-/archive/v4.7.1/libtiff-v4.7.1.tar.gz | tar xz
RUN tar -xvf libtiff-v4.7.1.tar.gz
WORKDIR /src/libtiff-v4.7.1
RUN ./autogen.sh
RUN emconfigure ./configure --with-pic --disable-webp --disable-zstd --disable-lzma --disable-jbig --disable-old-jpeg --disable-jpeg --disable-pixarlog --disable-mdi --disable-opengl --disable-win32-io --with-zlib-include-dir=/install/libs/include --with-zlib-lib-dir=/install/libs/lib --with-libdeflate-include-dir=/install/libs/include --with-libdeflate-lib-dir=/install/libs/lib/ --prefix=/install/libs --enable-shared=no
RUN emmake make -j && emmake make install
RUN ${LLVM_NM} /install/libs/lib/libtiff.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libtiff.exports

WORKDIR /src
# RUN curl -L https://www.sqlite.org/2025/sqlite-src-3500400.zip | unzip
RUN unzip sqlite-src-3500400.zip
WORKDIR /src/sqlite-src-3500400
RUN emconfigure ./configure --disable-shared --prefix=/install/libs --with-emsdk
RUN emmake make -j && emmake make install 
# we need a sqlite3 executable for the system on which we're building (needed by https://proj.org/en/stable/install.html#build-requirements)
RUN emmake make clean && ./configure --disable-shared && make -j && cp sqlite3 /tmp/sqlite3
RUN ${LLVM_NM} /install/libs/lib/libsqlite3.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libsqlite3.exports

WORKDIR /src
# RUN curl -L https://download.osgeo.org/proj/proj-9.7.0.tar.gz | tar xz
RUN tar -xvf proj-9.7.0.tar.gz
WORKDIR /src/proj-9.7.0
RUN emcmake cmake -DEMBED_RESOURCE_FILES=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX:PATH=/install/libs -DSQLite3_INCLUDE_DIR=/install/libs/include/ -DSQLite3_LIBRARY=/install/libs/lib/libsqlite3.a -DTIFF_LIBRARY=/install/libs/lib/libtiff.a -DTIFF_INCLUDE_DIR=/install/libs/include/ -DENABLE_CURL=OFF -DBUILD_APPS=OFF -DBUILD_TESTING=OFF -DEXE_SQLITE3=/tmp/sqlite3
RUN cmake --build . -j && cmake --build . --target install
RUN ${LLVM_NM} /install/libs/lib/libproj.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libproj.exports

WORKDIR /src
# RUN curl -L https://download.osgeo.org/geos/geos-3.14.0.tar.bz2 | tar xz
RUN tar -xvf geos-3.14.0.tar.bz2
WORKDIR /src/geos-3.14.0/build
RUN emcmake cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/install/libs -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_GEOSOP=OFF ..
RUN emmake make -j && emmake make install
RUN ${LLVM_NM} /install/libs/lib/libgeos.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libgeos.exports

WORKDIR /src
# RUN curl -L https://github.com/OSGeo/gdal/releases/download/v3.11.4/gdal-3.11.4.tar.gz | tar xz
RUN tar -xvf gdal-3.11.4.tar.gz
WORKDIR /src/gdal-3.11.4/build
# RUN emcmake cmake -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_APPS=OFF -DPROJ_LIBRARY=/install/libs/lib/libproj.a -DPROJ_INCLUDE_DIR=/install/libs/include/ -DCMAKE_PREFIX_PATH=/install/libs -DGDAL_BUILD_OPTIONAL_DRIVERS=OFF -DOGR_BUILD_OPTIONAL_DRIVERS=OFF -DCMAKE_INSTALL_PREFIX=/install/libs ..
# RUN emcmake cmake -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_APPS=OFF -DCMAKE_PREFIX_PATH=/install/libs -DGDAL_BUILD_OPTIONAL_DRIVERS=OFF -DOGR_BUILD_OPTIONAL_DRIVERS=OFF -DCMAKE_INSTALL_PREFIX=/install/libs ..
RUN emcmake cmake \
-DLIBXML2_INCLUDE_DIR=/install/libs/include/libxml2 -DLIBXML2_LIBRARY=/install/libs/lib/libxml2.a \
-DZLIB_LIBRARY=/install/libs/lib/libz.a -DZLIB_INCLUDE_DIR=/install/libs/include \
-DOPENSSL_ROOT_DIR=/install/libs \
-DPROJ_LIBRARY=/install/libs/lib/libproj.a -DPROJ_INCLUDE_DIR=/install/libs/include/ \
-DJSONC_LIBRARY=/install/libs/lib/libjson-c.a -DJSONC_INCLUDE_DIR=/install/libs/include/json-c -DGDAL_USE_JSONC=ON -DGDAL_USE_JSONC_INTERNAL=OFF \
-DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_APPS=OFF -DGDAL_BUILD_OPTIONAL_DRIVERS=OFF -DOGR_BUILD_OPTIONAL_DRIVERS=OFF -DCMAKE_INSTALL_PREFIX=/install/libs ..
RUN cmake --build . -j && cmake --build . --target install
RUN ${LLVM_NM} /install/libs/lib/libgdal.a | awk '$2 ~ /^[TDB]$/ {print $3}' | sed '/^$/d' | sort -u > /install/exports/libgdal.exports

ARG TARGETARCH
FROM emscripten/emsdk:${EMSDK_VER} AS base-amd64
FROM emscripten/emsdk:${EMSDK_VER}-arm64 AS base-arm64
FROM base-${TARGETARCH} AS runner

RUN apt update && apt upgrade -y && apt install -y \
    xz-utils autoconf libtool automake pkgconf bison flex vim

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