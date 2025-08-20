ARG EMSDK_VER=3.1.74
FROM emscripten/emsdk:${EMSDK_VER} AS builder

RUN apt update && apt upgrade -y && apt install -y \
    xz-utils autoconf libtool automake pkgconf bison flex

SHELL ["/bin/bash", "-c"]

WORKDIR /install/libs

# RUN embuilder --pic --verbose build zlib

RUN mkdir -p /install/libs

WORKDIR /src

COPY ./extern .

# ENV EMCC_COMMON_FLAGS="-fPIC -sWASM_BIGINT -sMIN_SAFARI_VERSION=150000 -D__PYDK__=1 -O2 -m32 -D_FILE_OFFSET_BITS=64 -sSUPPORT_LONGJMP=emscripten -mno-bulk-memory -mnontrapping-fptoint -mno-reference-types -mno-sign-ext -mno-extended-const -mno-atomics -mno-tail-call -mno-multivalue -mno-relaxed-simd -mno-simd128 -mno-multimemory -mno-exception-handling -Wno-unused-command-line-argument -Wno-unreachable-code-fallthrough -Wno-unused-function -Wno-invalid-noreturn -Wno-declaration-after-statement -Wno-invalid-noreturn"
ENV EMCC_COMMON_FLAGS="-O2 -fPIC"

WORKDIR /src
# RUN curl -L https://www.zlib.net/zlib-1.3.1.tar.gz | tar -xz
RUN tar -xvf zlib-1.3.1.tar.gz
WORKDIR /src/zlib-1.3.1
RUN CFLAGS="${EMCC_COMMON_FLAGS}" CXXFLAGS="${EMCC_COMMON_FLAGS}" emconfigure ./configure --static --prefix=/install/libs
RUN emmake make -j && emmake make install

WORKDIR /src
# RUN curl -L https://gitlab.gnome.org/GNOME/libxml2/-/archive/v2.14.5/libxml2-v2.14.5.tar.gz | tar -xz
RUN tar -xvf /src/libxml2-v2.14.5.tar.gz
WORKDIR /src/libxml2-v2.14.5
RUN ./autogen.sh --with-python=no
RUN CFLAGS="${EMCC_COMMON_FLAGS}" CXXFLAGS="${EMCC_COMMON_FLAGS}" emconfigure ./configure --enable-shared=no --enable-static=yes --with-python=no --prefix=/install/libs
RUN emmake make -j && emmake make install

WORKDIR /src
# RUN curl -L https://gitlab.gnome.org/GNOME/libxslt/-/archive/v1.1.43/libxslt-v1.1.43.tar.gz | tar -xz
RUN tar -xvf /src/libxslt-v1.1.43.tar.gz
WORKDIR /src/libxslt-v1.1.43
RUN ./autogen.sh --with-python=no
RUN CFLAGS="${EMCC_COMMON_FLAGS}" CXXFLAGS="${EMCC_COMMON_FLAGS}" emconfigure ./configure --enable-shared=no --enable-static=yes --with-python=no --prefix=/install/libs --with-libxml-src=/src/libxml2-v2.14.5/ --with-pic=yes
RUN emmake make -j && emmake make install

WORKDIR /src
# RUN curl -L https://github.com/openssl/openssl/releases/download/openssl-3.0.17/openssl-3.0.17.tar.gz | tar xz
RUN tar -xvf /src/openssl-3.0.17.tar.gz
WORKDIR /src/openssl-3.0.17
RUN emconfigure ./Configure no-tests linux-generic64 --prefix=/install/libs
RUN sed -i 's|^CROSS_COMPILE.*$|CROSS_COMPILE=|g' Makefile # see https://github.com/emscripten-core/emscripten/issues/19597#issue-1754476454
RUN emmake make -j && emmake make install


WORKDIR /src
# RUN curl -L ftp://ftp.ossp.org/pkg/lib/uuid/uuid-1.6.2.tar.gz | tar xz
RUN tar -xvf /src/uuid-1.6.2.tar.gz
WORKDIR /src/uuid-1.6.2
RUN CFLAGS="${EMCC_COMMON_FLAGS}" CXXFLAGS="${EMCC_COMMON_FLAGS}" emconfigure ./configure --enable-shared=no --enable-static=yes --with-perl=no --with-perl-compat=no --prefix=/install/libs --with-php=no --with-pic=yes
RUN emmake make -j && emmake make install || true # install tries to strip the wasm, but it doesnt recognize the format, so ignore atm
WORKDIR /install/libs/lib
RUN ln -s libuuid.a libossp-uuid.a # contrib extensions use -lossp-uuid

FROM emscripten/emsdk:${EMSDK_VER} AS runner

RUN apt update && apt upgrade -y && apt install -y \
    xz-utils autoconf libtool automake pkgconf bison flex

# this is where the libraries will be installed and subsequently where the LIBS and INCLUDES can be found
ARG INSTALL_PREFIX=/install/libs
ENV INSTALL_PREFIX=${INSTALL_PREFIX}

COPY --from=builder /install/libs ${INSTALL_PREFIX}

