FROM ubuntu:22.04 AS builder

RUN apt update && apt upgrade -y && apt install -y \
    xz-utils autoconf libtool automake pkgconf bison flex curl build-essential make

SHELL ["/bin/bash", "-c"]

RUN mkdir -p /install/libs

WORKDIR /src

ENV CXX_COMMON_FLAGS="-O2 -fPIC"

WORKDIR /src
RUN curl -L https://www.zlib.net/zlib-1.3.1.tar.gz | tar -xz
WORKDIR /src/zlib-1.3.1
RUN CFLAGS="${CXX_COMMON_FLAGS}" CXXFLAGS="${CXX_COMMON_FLAGS}" ./configure --static --prefix=/install/libs
RUN make -j && make install

WORKDIR /src
RUN curl -L https://gitlab.gnome.org/GNOME/libxml2/-/archive/v2.14.5/libxml2-v2.14.5.tar.gz | tar -xz
WORKDIR /src/libxml2-v2.14.5
RUN ./autogen.sh --with-python=no
RUN CFLAGS="${CXX_COMMON_FLAGS}" CXXFLAGS="${CXX_COMMON_FLAGS}" ./configure --enable-shared=no --enable-static=yes --with-python=no --prefix=/install/libs
RUN make -j && make install

WORKDIR /src
RUN curl -L https://gitlab.gnome.org/GNOME/libxslt/-/archive/v1.1.43/libxslt-v1.1.43.tar.gz | tar -xz
WORKDIR /src/libxslt-v1.1.43
RUN ./autogen.sh --with-python=no
RUN CFLAGS="${CXX_COMMON_FLAGS}" CXXFLAGS="${CXX_COMMON_FLAGS}" ./configure --enable-shared=no --enable-static=yes --with-python=no --prefix=/install/libs --with-libxml-src=/src/libxml2-v2.14.5/ --with-pic=yes
RUN make -j && make install

WORKDIR /src
RUN curl -L https://github.com/openssl/openssl/releases/download/openssl-3.0.17/openssl-3.0.17.tar.gz | tar xz
WORKDIR /src/openssl-3.0.17
RUN ./Configure no-tests linux-generic64 --prefix=/install/libs
# RUN sed -i 's|^CROSS_COMPILE.*$|CROSS_COMPILE=|g' Makefile # see https://github.com/emscripten-core/emscripten/issues/19597#issue-1754476454
RUN make -j && make install

WORKDIR /src
RUN curl -L ftp://ftp.ossp.org/pkg/lib/uuid/uuid-1.6.2.tar.gz | tar xz
# COPY . .
# RUN tar xvf uuid-1.6.2.tar.gz
WORKDIR /src/uuid-1.6.2
RUN CFLAGS="${CXX_COMMON_FLAGS}" CXXFLAGS="${CXX_COMMON_FLAGS}" ./configure --enable-shared=no --enable-static=yes --with-perl=no --with-perl-compat=no --prefix=/install/libs --with-php=no --with-pic=yes
RUN make -j && make install
WORKDIR /install/libs/lib
RUN ln -s libuuid.a libossp-uuid.a # contrib extensions use -lossp-uuid

FROM ubuntu:22.04 AS runner

RUN apt update && apt upgrade -y && apt install -y \
    xz-utils autoconf libtool automake pkgconf bison flex

# this is where the libraries will be installed and subsequently where the LIBS and INCLUDES can be found
ARG INSTALL_PREFIX=/install/libs
ENV INSTALL_PREFIX=${INSTALL_PREFIX}

COPY --from=builder /install/libs ${INSTALL_PREFIX}

# allow access to anyone 
RUN chmod -R 777 /install
