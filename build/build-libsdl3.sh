#!/bin/sh
# build-libsdl3.sh — Reproducible cross-build for libSDL3-pocketforge.so.0
#
# This is the canonical recipe for producing the libSDL3-pocketforge.so.0
# artifact that Steam Link binds against via SDL3_DYNAMIC_API and that
# PocketForge's own apps link directly. Lives in libsdl3-sunxifb so the
# build is reproducible from source-of-record alone.
#
# Updated in tsp-8si.13 (G4 work, 2026-06-11) to include the full subsystem
# coverage Steam Link requires. The earlier tsp-8si.10 recipe was over-
# trimmed (audio/joystick/render OFF) and Steam Link aborted at startup
# with `Couldn't initialize SDL: SDL not built with joystick support`. This
# recipe is the corrected baseline.
#
# Subsystems explicitly OFF: video backends our BSP can't drive (X11, Wayland,
# KMSDRM with no connectors, Vivante, RPi), Vulkan (no PowerVR Vulkan ICD),
# OpenGL (we use OpenGL ES through PowerVR), Camera/Tray (not needed), Static
# library (we ship shared only).
#
# Run inside pocketforge/cross-build:10.3-2021.07-bookworm with:
#   /work/src   <- libsdl3-sunxifb source tree (read-only)
#   /work/blobs <- pocketforge-os/blobs checkout (read-only)
#   /work/out   <- artifact output (writable, host-owned via --user)
#
# Example invocation from the libsdl3-sunxifb checkout root:
#   docker run --rm \
#     --user "$(id -u):$(id -g)" \
#     -v "$(pwd):/work/src:ro" \
#     -v "$(realpath ../blobs):/work/blobs:ro" \
#     -v "$(pwd)/_build:/work/out" \
#     pocketforge/cross-build:10.3-2021.07-bookworm \
#     bash /work/src/build/build-libsdl3.sh
set -eu

OUT=${OUT:-/work/out}
SRC=${SRC:-/work/src/sdl3}
BLOBS=${BLOBS:-/work/blobs/sunxi/a133/22.102.54.38}

# GPU model discriminator (tsp-mc9m.41.924.6 / C3): "ddk" (closed PowerVR DDK) or
# "open" (a133-open's Mesa GLES/EGL/GBM, Zink gallium) — no other value is valid, and
# there is deliberately no default (see the fail-closed comment below). CheckSUNXIFB
# (sdl3/cmake/sdlchecks.cmake) only ever uses SUNXIFB_DDK_ROOT as a generic
# CMAKE_FIND_ROOT_PATH entry to locate `include/EGL/egl.h` + `libEGL.so`/`libGLESv2.so`
# — it is not DDK-specific despite the name, so pointing it at the open Mesa install
# tree's prefix (which ships the same include/EGL + lib/libEGL.so shape) needs no
# cmake change at all.
# Fail CLOSED, not open (coordinator review finding on this PR): an UNKNOWN or EMPTY
# PF_GPU_MODEL must be a loud error, never a silent fall-through to the DDK branch —
# the same property B's PF_SOC validation enforces (platform/core/profile.py
# validate() now requires [device].soc so a broken profile errors loudly rather than
# resolving PF_SOC empty). Deliberately NO ":-ddk" default here (unlike this script's
# other env vars): the caller (Dockerfile.pf's sdl stage) always resolves and passes
# an explicit PF_GPU_MODEL, so requiring it here costs production nothing and closes
# the silent-typo path a bare default would otherwise leave open.
case "${PF_GPU_MODEL:-}" in
  open)
    DDK_ROOT="${SUNXIFB_MESA_ROOT:?SUNXIFB_MESA_ROOT must be set for PF_GPU_MODEL=open (the open Mesa install tree's prefix, e.g. .../usr/local — the C1 gpu-um-mesa stage's output)}"
    ;;
  ddk)
    # Pin DDK root unless caller already set it (multi-BVNC futures want override).
    DDK_ROOT="${SUNXIFB_DDK_ROOT:-${BLOBS}}"
    ;;
  *)
    echo "FATAL: PF_GPU_MODEL must be 'ddk' or 'open', got '${PF_GPU_MODEL:-}' (unset/empty counts as invalid)" >&2
    exit 1
    ;;
esac

cmake -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/opt/cmake/toolchain-arm-10.3-2021.07.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SKIP_RPATH=ON \
  -DSUNXIFB_DDK_ROOT="${DDK_ROOT}" \
  \
  -DSDL_SUNXIFB=ON \
  -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF \
  -DSDL_VIVANTE=OFF -DSDL_RPI=OFF -DSDL_OFFSCREEN=OFF \
  -DSDL_DUMMYVIDEO=ON \
  \
  -DSDL_AUDIO=ON -DSDL_RENDER=ON -DSDL_GPU=ON \
  -DSDL_JOYSTICK=ON -DSDL_HAPTIC=ON -DSDL_HIDAPI=ON \
  -DSDL_POWER=ON -DSDL_SENSOR=ON -DSDL_DIALOG=ON \
  -DSDL_CAMERA=OFF -DSDL_TRAY=OFF \
  \
  -DSDL_OPENGL=OFF -DSDL_OPENGLES=ON -DSDL_VULKAN=OFF \
  -DSDL_ALSA=ON -DSDL_PULSEAUDIO=ON \
  \
  -DSDL_UNIX_CONSOLE_BUILD=ON \
  -DSDL_RPATH=OFF \
  -DSDL_TESTS=ON \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF \
  \
  -B "${OUT}" -S "${SRC}"

cmake --build "${OUT}" -j"$(nproc)"

echo
echo "=== artifact verification ==="
SO="${OUT}/libSDL3-pocketforge.so.0.5.0"
sha256sum "${SO}"
"$(dirname "$0")/verify-libsdl3-artifact.sh" "${SO}"
echo "OK"
