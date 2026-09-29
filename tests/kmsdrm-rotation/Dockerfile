# tsp-mc9m.41.924.16.12: toolchain for the hermetic KMSDRM rotated-present
# tests (run.sh). Native x86-64 build of this SDL tree plus Mesa's llvmpipe for
# the pixel test; no device, no DRM node, no network at run time.
FROM debian:bookworm-slim

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libdrm-dev \
        libegl-dev \
        libegl-mesa0 \
        libegl1 \
        libgbm-dev \
        libgl1-mesa-dri \
        libgles-dev \
        libgles2 \
        ninja-build \
        pkg-config \
 && rm -rf /var/lib/apt/lists/*
