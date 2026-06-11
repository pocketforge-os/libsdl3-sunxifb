# syntax=docker/dockerfile:1.6
#
# PocketForge cross-build container — ARM A-Profile Toolchain 10.3-2021.07
# ===========================================================================
#
# Produces aarch64 binaries that load cleanly against TSP's glibc 2.33.
# Do NOT replace the toolchain with `apt install gcc-aarch64-linux-gnu` — that
# is gcc-13/glibc 2.34 and produces binaries that fail at startup on TSP with
# `version 'GLIBC_2.34' not found` (see hardware-firmware-probes.md §11.5).
#
# Build:
#   docker build -t pocketforge/cross-build:10.3-2021.07-bookworm build/
#
# Run (artifacts owned by host user, no root in /work/out):
#   docker run --rm \
#     --user "$(id -u):$(id -g)" \
#     -v "$PWD/src:/work/src:ro" \
#     -v "$PWD/blobs:/work/blobs:ro" \
#     -v "$PWD/out:/work/out" \
#     pocketforge/cross-build:10.3-2021.07-bookworm \
#     bash -c "<build commands>"
#
# Triplet: aarch64-none-linux-gnu  (note `-none-`, not Linux-conventional
# `-linux-`). SDL3's CMake doesn't care; do NOT manufacture short-name
# symlinks — the toolchain ships zero by design.
#
# CMake toolchain file: /opt/cmake/toolchain-arm-10.3-2021.07.cmake
# Sysroot: /opt/arm-10.3-2021.07/aarch64-none-linux-gnu/libc
# (includes glibc 2.33 + drm headers + pthread.h; NO EGL/GLES2 — those come
# from the PowerVR DDK in the blobs repo, bind-mounted at /work/blobs).
#
# ---------------------------------------------------------------------------

# =============================================================================
# Stage 1 — toolchain-fetch
# Single-purpose stage: fetch + verify + unpack the ARM toolchain. Layered
# separately so changes to apt deps in stage 2 don't re-download the 250 MB
# tarball.
# =============================================================================
FROM debian:bookworm-slim AS toolchain-fetch

ARG TOOLCHAIN_VERSION=10.3-2021.07
ARG TOOLCHAIN_TRIPLET=aarch64-none-linux-gnu
ARG TOOLCHAIN_TARBALL=gcc-arm-${TOOLCHAIN_VERSION}-x86_64-${TOOLCHAIN_TRIPLET}.tar.xz
ARG TOOLCHAIN_URL=https://armkeil.blob.core.windows.net/developer/Files/downloads/gnu-a/${TOOLCHAIN_VERSION}/binrel/${TOOLCHAIN_TARBALL}

# Dual-pin: SHA-256 (computed locally 2026-06-10) + MD5 (Arm-published).
# If Arm rotates the URL, do NOT silently substitute — see
# build-integration-reference.md §3.6 for the archived URL.
ARG TOOLCHAIN_SHA256=1e33d53dea59c8de823bbdfe0798280bdcd138636c7060da9d77a97ded095a84
ARG TOOLCHAIN_MD5=07bbe2b5277b75ba36a924e9136366a4

# Minimal deps for fetch + extract + verify. xz-utils handles .tar.xz; ca-certs
# is non-negotiable for HTTPS to armkeil.blob.core.windows.net.
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        xz-utils \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp/toolchain

# Fetch + dual-checksum. Both must pass; either failure aborts the build.
# `sha256sum -c` and `md5sum -c` exit non-zero on mismatch.
RUN set -eux; \
    curl --fail --silent --show-error --location \
         --output "${TOOLCHAIN_TARBALL}" \
         "${TOOLCHAIN_URL}"; \
    echo "${TOOLCHAIN_SHA256}  ${TOOLCHAIN_TARBALL}" | sha256sum -c -; \
    echo "${TOOLCHAIN_MD5}  ${TOOLCHAIN_TARBALL}" | md5sum -c -

# Unpack to a deterministic install prefix. The tarball's top-level directory
# is `gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/`; we rename to
# `arm-${TOOLCHAIN_VERSION}` so downstream paths are short and version-pinned.
RUN set -eux; \
    mkdir -p /opt; \
    tar -xJf "${TOOLCHAIN_TARBALL}" -C /opt; \
    mv "/opt/gcc-arm-${TOOLCHAIN_VERSION}-x86_64-${TOOLCHAIN_TRIPLET}" \
       "/opt/arm-${TOOLCHAIN_VERSION}"; \
    rm -f "${TOOLCHAIN_TARBALL}"

# Strip ~45 MB of docs/manpages we'll never read inside the container.
# The compiler binaries and the sysroot stay untouched.
RUN set -eux; \
    rm -rf "/opt/arm-${TOOLCHAIN_VERSION}/share/doc" \
           "/opt/arm-${TOOLCHAIN_VERSION}/share/man" \
           "/opt/arm-${TOOLCHAIN_VERSION}/share/info"

# Smoke test the toolchain at fetch time so a corrupt extract surfaces here,
# not in stage 2 or in user code. Just check the compiler runs.
RUN "/opt/arm-${TOOLCHAIN_VERSION}/bin/${TOOLCHAIN_TRIPLET}-gcc" --version | \
        grep -F "10.3-2021.07"

# =============================================================================
# Stage 2 — final
# Inherits the unpacked toolchain via COPY --from. apt-installs everything we
# need across Phase 0 (SDL3 build) and Phase 1 (image composition) so we don't
# need a second container later.
# =============================================================================
FROM debian:bookworm-slim AS final

ARG TOOLCHAIN_VERSION=10.3-2021.07
ARG TOOLCHAIN_TRIPLET=aarch64-none-linux-gnu

# Phase 0+1 build-deps in one apt transaction. Categorized for review:
#
#   Cross-build basics:   cmake ninja-build pkg-config make patch xxd
#   Source acquisition:   git curl ca-certificates gpg
#   Archive handling:     xz-utils
#   DT compilation:       device-tree-compiler  (provides `dtc` binary)
#   Image composition:    abootimg genimage dosfstools mtools e2fsprogs
#                         sunxi-tools
#   Debian rootfs:        debootstrap qemu-user-static binfmt-support
#   Scripting glue:       python3
#
# `--no-install-recommends` keeps the layer slim; `apt-get clean` + lists rm
# at end of layer for reproducibility.
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        ninja-build \
        pkg-config \
        git \
        curl \
        gpg \
        make \
        patch \
        xxd \
        xz-utils \
        device-tree-compiler \
        abootimg \
        genimage \
        dosfstools \
        mtools \
        e2fsprogs \
        sunxi-tools \
        debootstrap \
        qemu-user-static \
        binfmt-support \
        python3 \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/*

# Pull the unpacked toolchain across from stage 1.
COPY --from=toolchain-fetch /opt/arm-${TOOLCHAIN_VERSION} /opt/arm-${TOOLCHAIN_VERSION}

# PATH wiring: prepend toolchain bin/ for both login and non-login shells.
# /etc/profile.d/ handles login shells; ENV PATH handles `docker run ...
# bash -c "..."` invocations that don't source profile.
ENV PATH=/opt/arm-${TOOLCHAIN_VERSION}/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
RUN printf '%s\n' \
        '# PocketForge cross-toolchain (10.3-2021.07)' \
        'export PATH=/opt/arm-10.3-2021.07/bin:$PATH' \
        > /etc/profile.d/cross-toolchain.sh \
 && chmod +x /etc/profile.d/cross-toolchain.sh

# CMake toolchain file. CMAKE_FIND_ROOT_PATH_MODE_* is set up so that
# find_package() / find_library() see the sysroot for libs/headers but
# always use host programs (programs build-time, not runtime). The
# CMAKE_FIND_ROOT_PATH list is multi-element to anticipate the PowerVR DDK
# bind-mount at /work/blobs becoming a second find-root in downstream beads.
RUN mkdir -p /opt/cmake
COPY toolchain-arm-10.3-2021.07.cmake /opt/cmake/toolchain-arm-10.3-2021.07.cmake

# Convenience symlink for the latest toolchain CMake file. Keeps user-facing
# invocations short while preserving the version-pinned canonical path.
RUN ln -sf /opt/cmake/toolchain-arm-10.3-2021.07.cmake /opt/cmake/toolchain.cmake

# -----------------------------------------------------------------------------
# Build-time validation gate
# -----------------------------------------------------------------------------
# Hello-world a tiny program with the toolchain and verify (a) it runs, (b)
# the produced ELF references no GLIBC_2.34+ symbols. If this RUN fails, the
# image build aborts before publication. Defense in depth: the same check is
# duplicated in build/check-glibc-symver.sh for CI use against real artifacts.
COPY check-glibc-symver.sh /usr/local/bin/check-glibc-symver
RUN set -eux; \
    chmod +x /usr/local/bin/check-glibc-symver; \
    apt-get update; \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        binutils-aarch64-linux-gnu; \
    rm -rf /var/lib/apt/lists/*; \
    mkdir -p /tmp/symver-check; \
    cd /tmp/symver-check; \
    printf '%s\n' \
        '#include <stdio.h>' \
        'int main(void) { puts("ok"); return 0; }' \
        > hello.c; \
    "${TOOLCHAIN_TRIPLET}-gcc" -O2 -o hello hello.c; \
    /usr/local/bin/check-glibc-symver hello; \
    cd /; \
    rm -rf /tmp/symver-check

# Default workdir matches the documented bind-mount layout: build commands
# `cd /work/src` (read-only source), produce into `/work/out`.
WORKDIR /work

# No CMD/ENTRYPOINT — invoked with explicit `bash -c "..."` per usage docs.
# Image labels for traceability.
LABEL org.opencontainers.image.title="pocketforge-cross-build" \
      org.opencontainers.image.description="ARM A-Profile Toolchain 10.3-2021.07 + image-composition tools for PocketForge / TrimUI Smart Pro" \
      org.opencontainers.image.version="10.3-2021.07-bookworm" \
      org.opencontainers.image.source="https://github.com/pocketforge-os/libsdl3-sunxifb"
