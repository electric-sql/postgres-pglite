FROM ubuntu:22.04 AS build_sdk

ARG PG_VERSION=16.4
ARG SDK_VERSION=3.1.74.3bi
ARG DEBUG=false
ARG OBJDUMP=true

ENV \
  PG_VERSION=$PG_VERSION \
  SDK_VERSION=$SDK_VERSION \
  SDKROOT=/opt/python-wasm-sdk \
  SYS_PYTHON=/usr/bin/python3 \
  DEBUG=$DEBUG \
  BUILDS=3.12 \
  EMFLAVOUR=3.1.74

WORKDIR /workspace

# Install dependencies to build python-wasm-sdk
RUN \
  apt-get update &&\
  apt-get install -y \
  sudo patchelf git clang unzip autoconf libtool \
  libsqlite3-dev libffi-dev libssl-dev zlib1g-dev pkg-config \
  libncursesw5-dev python3 python3-pip \
  make build-essential wget lz4 bzip2 pv curl

# Download the python-wasm-sdk source for the given version
# RUN git clone --depth 1 --branch ${SDK_VERSION} https://github.com/pygame-web/python-wasm-sdk.git
RUN git clone --depth 1 https://github.com/electric-sql/portable-sdk.git

WORKDIR /workspace/portable-sdk/python-wasm-sdk

# Make python-wasm-sdk
RUN chmod +x python-wasm-sdk.sh 

RUN bash -c "./python-wasm-sdk.sh"

FROM ubuntu:22.04

# Install dependencies to build postgres wasm
RUN \
  apt-get update &&\
  apt-get install -y lz4 wget pv bash bzip2 python3 build-essential libreadline-dev zlib1g-dev bison flex &&\
  apt-get clean

ARG PG_VERSION=16.4
ARG SDK_VERSION=3.1.74.3bi
ARG DEBUG=false
ARG OBJDUMP=true

ENV \
  PG_VERSION=$PG_VERSION \
  SDK_VERSION=$SDK_VERSION \
  SDK_ARCHIVE=python3.12-wasm-sdk-Ubuntu-22.04.tar.lz4 \
  SDKROOT=/opt/python-wasm-sdk \
  SYS_PYTHON=/usr/bin/python3 \
  PGROOT=/tmp/pglite \
  DEBUG=$DEBUG \
  OBJDUMP=$OBJDUMP

COPY --from=build_sdk /tmp/sdk /tmp/sdk

RUN tar --use-compress-program=lz4 -xvf /tmp/sdk/${SDK_ARCHIVE} -C /

RUN rm -r /tmp/sdk

WORKDIR /workspace