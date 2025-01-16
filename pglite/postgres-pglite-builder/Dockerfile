FROM ubuntu:22.04

ARG PG_VERSION
ARG SDK_VERSION
ARG DEBUG=false
ARG OBJDUMP=true

ENV \
  PG_VERSION=$PG_VERSION \
  SDK_VERSION=$SDK_VERSION \
  SDK_ARCHIVE=python3.12-wasm-sdk-Ubuntu-22.04.tar \
  SDKROOT=/opt/python-wasm-sdk \
  SYS_PYTHON=/usr/bin/python3 \
  PGROOT=/tmp/pglite \
  DEBUG=$DEBUG \
  OBJDUMP=$OBJDUMP

WORKDIR /workspace

# ENV SDK_VERSION=3.1.74.2bi
ENV WASI_SDK_VERSION=24.0.5
ENV SDK_ARCHIVE=python3.13-wasm-sdk-Ubuntu-22.04.tar.lz4
ENV WASI_SDK_ARCHIVE=python3.13-wasi-sdk-Ubuntu-22.04.tar.lz4

# Install dependencies to build postgres wasm
RUN \
  apt-get update &&\
  apt-get install -y lz4 wget pv bash curl bzip2 python3 build-essential libreadline-dev zlib1g-dev bison flex git curl &&\
  apt-get clean

RUN curl -sL --retry 5 https://github.com/pygame-web/python-wasm-sdk/releases/download/$SDK_VERSION/$SDK_ARCHIVE | tar xvP --use-compress-program=lz4 | pv -p -l -s 46000 >/dev/null
RUN curl -sL --retry 5 https://github.com/pygame-web/python-wasi-sdk/releases/download/$WASI_SDK_VERSION/$WASI_SDK_ARCHIVE | tar xvP --use-compress-program=lz4 | pv -p -l -s 23000 >/dev/null

# Extract SDK
RUN cd / && tar xvf /tmp/sdk/${SDK_ARCHIVE} | pv -p -l -s 24400 >/dev/null

# Clean up packaged SDK
RUN rm -rf /tmp/sdk
