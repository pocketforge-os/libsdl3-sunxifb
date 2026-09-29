# tsp-f3fm.218: test layer over the pinned cross-build container (build/Dockerfile).
# The cross container carries only the aarch64 toolchain; the fixture contract
# (build/test-build-libsdl3-contract.sh) also compiles host ELF controls, so add
# the host compiler and binutils. Nothing else changes: run.sh drives the real
# build-libsdl3.sh with the pinned toolchain file inside this image.
ARG CROSS_IMAGE
FROM ${CROSS_IMAGE}

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        binutils \
        build-essential \
 && rm -rf /var/lib/apt/lists/*
