#!/usr/bin/env bash
# tsp-f3fm.218: the GPU model selects the compiled SDL display backend, proven
# on real built libraries.
#
# Runs inside tests/gpu-model-backends/Dockerfile (the pinned cross-build
# container plus a host compiler) with this repository mounted read-only at
# $SRC and no network (CI: .github/workflows/gpu-model-backends.yml).
#
#   1. fixture   build/test-build-libsdl3-contract.sh (stubbed tools and host
#                ELF controls).
#   2. open      the real build/build-libsdl3.sh, PF_GPU_MODEL=open, with the
#                pinned toolchain file. The built library must contain KMSDRM
#                and no sunxifb, plus dynamic Wayland: checked by the build's
#                own verifier and again here from the build config, the symbol
#                table, the strings and SDL's own runtime video-driver list
#                (run under qemu-aarch64).
#   3. ddk       the same for PF_GPU_MODEL=ddk: Wayland plus sunxifb, no KMSDRM.
#   4. red       the base commit's build logic: the fixture contract must fail
#                against it, and the closed-DDK singleton it builds must fail
#                this
#                tree's verifier because it has no Wayland bootstrap (the
#                production contract that broke Pool Suite). Skipped once the base has
#                the fix.
#
# Only the vendor graphics input is substituted. The real DDK (pocketforge-os/
# blobs) and the open Mesa tree are inputs of the image build, not of this
# repository, so both models build against one stub EGL/GLES root: SDL's own
# Khronos headers plus libEGL.so.1/libGLESv2.so.2 stubs built with the pinned
# toolchain. Which backends compile depends on the CMake options and on those
# headers and libraries being present, not on their implementation.
set -euo pipefail

SRC=${SRC:-/src}
WORK=${WORK:-/tmp/gpu-model-backends}
BASE_SHA=${BASE_SHA:-}
TRIPLET=aarch64-none-linux-gnu
SYSROOT=/opt/arm-10.3-2021.07/${TRIPLET}/libc
SO_NAME=libSDL3-pocketforge.so.0.5.0

if [ ! -f /.dockerenv ] && [ "${GPU_MODEL_BACKENDS_IN_CONTAINER:-}" != 1 ]; then
    echo "run.sh: refusing to run outside the pinned cross-build container" >&2
    exit 2
fi
for tool in "${TRIPLET}-gcc" "${TRIPLET}-nm" "${TRIPLET}-strings" "${TRIPLET}-readelf" \
    qemu-aarch64-static cmake ninja cc git; do
    command -v "$tool" >/dev/null || { echo "run.sh: missing $tool" >&2; exit 2; }
done

step() { printf '\n=== %s ===\n' "$*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK"
git config --global --add safe.directory "$SRC" >/dev/null 2>&1 || true

step "1. fixture: build/test-build-libsdl3-contract.sh"
sh "$SRC/build/test-build-libsdl3-contract.sh"

step "stub EGL/GLES root (SDL Khronos headers, pinned-toolchain stub libraries)"
EGL_ROOT=$WORK/egl-root
mkdir -p "$EGL_ROOT/include" "$EGL_ROOT/lib"
cp -a "$SRC/sdl3/src/video/khronos/EGL" "$SRC/sdl3/src/video/khronos/KHR" \
      "$SRC/sdl3/src/video/khronos/GLES" "$SRC/sdl3/src/video/khronos/GLES2" \
      "$SRC/sdl3/src/video/khronos/GLES3" "$EGL_ROOT/include/"
# One no-op definition per prototype the headers declare.
stub_library() {
    local soname=$1 link=$2 marker=$3
    shift 3
    grep -hE "^${marker} " "$@" \
        | sed -nE 's/.*(EGLAPIENTRY|GL_APIENTRY) +([A-Za-z0-9_]+) *\(.*/\2/p' \
        | sort -u | sed 's/.*/void &(void) {}/' >"$WORK/${soname}.c"
    [ -s "$WORK/${soname}.c" ] || fail "no prototypes found for ${soname}"
    "${TRIPLET}-gcc" -shared -fPIC -Wl,-soname,"$soname" \
        -o "$EGL_ROOT/lib/$soname" "$WORK/${soname}.c"
    ln -sf "$soname" "$EGL_ROOT/lib/$link"
    echo "$soname: $(wc -l <"$WORK/${soname}.c") stub functions"
}
stub_library libEGL.so.1 libEGL.so EGLAPI "$EGL_ROOT"/include/EGL/egl.h "$EGL_ROOT"/include/EGL/eglext.h
stub_library libGLESv2.so.2 libGLESv2.so GL_APICALL "$EGL_ROOT"/include/GLES2/gl2.h "$EGL_ROOT"/include/GLES2/gl2ext.h

# The runtime video-driver list, from SDL itself, under qemu-aarch64.
cat >"$WORK/list-video-drivers.c" <<'EOF'
#include <stdio.h>
extern int SDL_GetNumVideoDrivers(void);
extern const char *SDL_GetVideoDriver(int index);
int main(void)
{
    int n = SDL_GetNumVideoDrivers();
    for (int i = 0; i < n; i++) {
        printf("%s\n", SDL_GetVideoDriver(i));
    }
    return n > 0 ? 0 : 1;
}
EOF
video_drivers() {
    local out=$1
    "${TRIPLET}-gcc" -std=c99 -O1 -o "$out/list-video-drivers" "$WORK/list-video-drivers.c" \
        -L"$out" -lSDL3-pocketforge -Wl,-rpath-link,"$out:$EGL_ROOT/lib"
    QEMU_LD_PREFIX=$SYSROOT LD_LIBRARY_PATH="$out:$EGL_ROOT/lib" \
        qemu-aarch64-static "$out/list-video-drivers"
}

# build_model <model> <sdl tree> <out>: the tree's own build/build-libsdl3.sh.
build_model() {
    local model=$1 tree=$2 out=$3
    mkdir -p "$out"
    if ! OUT=$out SRC=$tree/sdl3 PF_GPU_MODEL=$model \
        SUNXIFB_MESA_ROOT=$EGL_ROOT SUNXIFB_DDK_ROOT=$EGL_ROOT \
        sh "$tree/build/build-libsdl3.sh" >"$out.log" 2>&1; then
        grep -nE 'FATAL|error' "$out.log" | head -n 40 >&2 || true
        tail -n 40 "$out.log" >&2
        fail "$model build from $tree failed"
    fi
    sed -n '/=== artifact verification ===/,$p' "$out.log"
}

# The facts one built library states about its display backends.
inspect() {
    local out=$1 so=$1/$SO_NAME config
    config=$(find "$out" -name SDL_build_config.h -path '*include-config*' | head -n 1)
    [ -n "$config" ] || fail "no SDL_build_config.h under $out"
    grep -Eo '^#define SDL_VIDEO_DRIVER_(SUNXIFB|KMSDRM|WAYLAND) 1' "$config" \
        | sed 's/^#define /config: /;s/ 1$//' || true
    "${TRIPLET}-nm" "$so" | awk 'NF { print $NF }' | grep -Ex '(SUNXIFB|KMSDRM|Wayland)_bootstrap' \
        | sed 's/^/symbol: /' || true
    "${TRIPLET}-strings" "$so" | grep -Ex 'wayland|sunxifb|kmsdrm' | sort -u | sed 's/^/string: /' || true
    "${TRIPLET}-strings" "$so" \
        | grep -Ex 'libwayland-(client|egl|cursor)\.so\.[0-9]+|libxkbcommon\.so\.[0-9]+' \
        | sort -u | sed 's/^/runtime: /' || true
    "${TRIPLET}-readelf" -d "$so" | grep -F '(NEEDED)' | grep -Eo '\[lib(EGL|GLESv2|xkbcommon|wayland-[^.]*)\.so[^]]*\]' \
        | sed 's/^/needed: /' || true
    video_drivers "$out" | sed 's/^/driver: /'
}

# expect <facts file> present|absent <line>
expect() {
    local facts=$1 want=$2 line=$3
    if grep -Fxq -- "$line" "$facts"; then
        [ "$want" = present ] || fail "$(basename "$facts"): unexpected '$line'"
    else
        [ "$want" = absent ] || fail "$(basename "$facts"): missing '$line'"
    fi
}

step "2. open: real build-libsdl3.sh, PF_GPU_MODEL=open"
build_model open "$SRC" "$WORK/out-open"
inspect "$WORK/out-open" | tee "$WORK/facts-open"
for line in 'config: SDL_VIDEO_DRIVER_WAYLAND' 'symbol: Wayland_bootstrap' 'string: wayland' 'driver: wayland' \
    'runtime: libwayland-client.so.0' 'runtime: libwayland-egl.so.1' \
    'runtime: libwayland-cursor.so.0' 'runtime: libxkbcommon.so.0' \
    'config: SDL_VIDEO_DRIVER_KMSDRM' 'symbol: KMSDRM_bootstrap' 'string: kmsdrm' 'driver: kmsdrm'; do
    expect "$WORK/facts-open" present "$line"
done
for line in 'config: SDL_VIDEO_DRIVER_SUNXIFB' 'symbol: SUNXIFB_bootstrap' 'string: sunxifb' 'driver: sunxifb' \
    'needed: [libEGL.so.1]' 'needed: [libGLESv2.so.2]'; do
    expect "$WORK/facts-open" absent "$line"
done
if grep -E '^needed: \[lib(wayland-|xkbcommon)' "$WORK/facts-open"; then
    fail "open: Wayland runtime libraries are directly linked"
fi
order=$(sed -n 's/^driver: //p' "$WORK/facts-open" | grep -Ex 'wayland|kmsdrm' | paste -sd' ')
[ "$order" = 'wayland kmsdrm' ] || fail "open: expected Wayland then KMSDRM, got '$order'"
echo "ok: the open library tries dynamic Wayland before KMSDRM and contains no sunxifb"

step "3. ddk: real build-libsdl3.sh, PF_GPU_MODEL=ddk"
build_model ddk "$SRC" "$WORK/out-ddk"
inspect "$WORK/out-ddk" | tee "$WORK/facts-ddk"
for line in 'config: SDL_VIDEO_DRIVER_WAYLAND' 'symbol: Wayland_bootstrap' 'string: wayland' 'driver: wayland' \
    'runtime: libwayland-client.so.0' 'runtime: libwayland-egl.so.1' \
    'runtime: libwayland-cursor.so.0' 'runtime: libxkbcommon.so.0' \
    'config: SDL_VIDEO_DRIVER_SUNXIFB' 'symbol: SUNXIFB_bootstrap' 'string: sunxifb' 'driver: sunxifb' \
    'needed: [libEGL.so.1]' 'needed: [libGLESv2.so.2]'; do
    expect "$WORK/facts-ddk" present "$line"
done
for line in 'config: SDL_VIDEO_DRIVER_KMSDRM' 'symbol: KMSDRM_bootstrap' 'string: kmsdrm' 'driver: kmsdrm'; do
    expect "$WORK/facts-ddk" absent "$line"
done
if grep -E '^needed: \[lib(wayland-|xkbcommon)' "$WORK/facts-ddk"; then
    fail "ddk: Wayland runtime libraries are directly linked"
fi
order=$(sed -n 's/^driver: //p' "$WORK/facts-ddk" | grep -Ex 'wayland|sunxifb' | paste -sd' ')
[ "$order" = 'wayland sunxifb' ] || fail "ddk: expected Wayland then sunxifb, got '$order'"
echo "ok: the ddk library tries dynamic Wayland before sunxifb and contains no KMSDRM"

step "4. red: the base commit's build logic"
# shellcheck disable=SC2016  # the literal recipe text, not an expansion
FIXED_RECIPE='-DSDL_WAYLAND=ON'
if [ -z "$BASE_SHA" ] || ! git -C "$SRC" cat-file -e "$BASE_SHA^{commit}" 2>/dev/null; then
    echo "SKIP: no base commit given (BASE_SHA)"
elif git -C "$SRC" show "$BASE_SHA:build/build-libsdl3.sh" | grep -F -- "$FIXED_RECIPE" >/dev/null; then
    echo "SKIP: base $BASE_SHA already enables the Wayland video backend"
else
    base=$WORK/base-src
    mkdir -p "$base"
    git -C "$SRC" archive "$BASE_SHA" build sdl3 | tar -x -C "$base"

    # This tree's fixture contract against the base build scripts.
    cp "$SRC/build/test-build-libsdl3-contract.sh" "$base/build/"
    if sh "$base/build/test-build-libsdl3-contract.sh" >"$WORK/red-fixture.log" 2>&1; then
        fail "the fixture contract passed against the base build logic; it does not detect the defect"
    fi
    grep -E '^FAIL: ' "$WORK/red-fixture.log"

    # The base closed-DDK singleton: its own verifier accepts it, this tree's must not.
    build_model ddk "$base" "$WORK/out-base-ddk"
    inspect "$WORK/out-base-ddk" | tee "$WORK/facts-base-ddk"
    expect "$WORK/facts-base-ddk" absent 'symbol: Wayland_bootstrap'
    expect "$WORK/facts-base-ddk" absent 'driver: wayland'
    expect "$WORK/facts-base-ddk" present 'symbol: SUNXIFB_bootstrap'
    expect "$WORK/facts-base-ddk" present 'driver: sunxifb'
    if PF_GPU_MODEL=ddk "$SRC/build/verify-libsdl3-artifact.sh" "$WORK/out-base-ddk/$SO_NAME" \
        >"$WORK/red-verify.log" 2>&1; then
        fail "this tree's verifier accepted the base closed-DDK singleton"
    fi
    grep -F 'FATAL: SDL artifact is missing the Wayland_bootstrap symbol' "$WORK/red-verify.log" \
        || { cat "$WORK/red-verify.log" >&2; fail "the base closed-DDK singleton failed verification, but not on Wayland"; }
    echo "ok: the base closed-DDK singleton lacks Wayland (the production defect) and this tree's verifier refuses it"
fi

step "RESULT: PASS"
