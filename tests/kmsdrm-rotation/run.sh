#!/usr/bin/env bash
# tsp-mc9m.41.924.16.12: hermetic tests of the KMSDRM rotated present (a panel
# whose connector "panel orientation" is not Normal).
#
# Runs inside the container from tests/kmsdrm-rotation/Dockerfile, with this
# repository mounted read-only at $SRC (CI: .github/workflows/kmsdrm-rotation.yml).
# It creates the regular file /dev/dri/card0 for SDL to open, so it refuses to
# run outside a container.
#
#   1. unit      SDL_kmsdrmorientation.h against the kernel names, pf-framehost's
#                pixel table and the panel corners;
#   2. llvmpipe  the rotate pass's real GLES2 code, every pixel, 90/270/180/0;
#   3. backend   an SDL app on this tree's KMSDRM backend over fake-kms.c, for
#                each present path (atomic fenced, atomic double-buffered,
#                legacy) and orientation, including the Normal negative control
#                and the SDL_KMSDRM_PRESENT_ROTATION=0 opt-out, a stack
#                missing a GL entry point mid-table (window creation must fail
#                cleanly), a failing final eglMakeCurrent in a surface rebuild
#                (nothing may leak), and Vulkan windows (panel-native: the
#                application owns the panel orientation);
#   4. red       the same app on the base commit's SDL must fail for a panel
#                reporting Left Side Up (skipped once the base has the fix);
#   5. red       the MakeCurrent-failure scenario on a71a8dab, the commit before
#                the surface-rebuild cleanup fix, must fail on its leak
#                (skipped when that commit is not in the checkout).
set -euo pipefail

SRC=${SRC:-/src}
WORK=${WORK:-/tmp/kmsdrm-rotation}
BASE_SHA=${BASE_SHA:-}
T="$SRC/tests/kmsdrm-rotation"
KMSDRM="$SRC/sdl3/src/video/kmsdrm"
NOX11=(-DEGL_NO_X11 -DMESA_EGL_NO_X11_HEADERS)
STRICT=(-std=c99 -O1 -g -Wall -Wextra -Werror "${NOX11[@]}")

if [ ! -f /.dockerenv ] && [ "${KMSDRM_ROTATION_IN_CONTAINER:-}" != 1 ]; then
    echo "run.sh: refusing to run outside a container (it creates /dev/dri/card0)" >&2
    exit 2
fi

step() { printf '\n=== %s ===\n' "$*"; }

rm -rf "$WORK"
mkdir -p "$WORK"
git config --global --add safe.directory "$SRC" >/dev/null 2>&1 || true

step "1. unit: orientation map"
cc "${STRICT[@]}" -I"$KMSDRM" -o "$WORK/test-orientation" "$T/test-orientation.c"
"$WORK/test-orientation"

step "2. llvmpipe: rotate pass pixels"
cc "${STRICT[@]}" -I"$KMSDRM" -o "$WORK/test-rotategl" "$T/test-rotategl.c" -lEGL -lGLESv2
LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless "$WORK/test-rotategl"

build_sdl() {
    local src=$1 name=$2
    local out="$WORK/build-$name" prefix="$WORK/prefix-$name"
    if ! cmake -S "$src/sdl3" -B "$out" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_SUNXIFB=OFF \
        -DSDL_KMSDRM=ON -DSDL_KMSDRM_SHARED=ON -DSDL_OPENGL=OFF -DSDL_OPENGLES=ON -DSDL_VULKAN=ON \
        -DSDL_AUDIO=OFF -DSDL_CAMERA=OFF -DSDL_RPATH=OFF >"$out.configure.log" 2>&1; then
        tail -n 60 "$out.configure.log"
        return 1
    fi
    if ! grep -rqs 'define SDL_VIDEO_DRIVER_KMSDRM_DYNAMIC ' "$out"; then
        echo "SDL ($name) was configured without dynamically loaded KMSDRM" >&2
        grep -iE 'kmsdrm|gbm|libdrm' "$out.configure.log" | tail -n 20 >&2 || true
        return 1
    fi
    if ! cmake --build "$out" >"$out.build.log" 2>&1; then
        grep -nE 'error' "$out.build.log" | head -n 60
        tail -n 30 "$out.build.log"
        return 1
    fi
    cmake --install "$out" >/dev/null
}

# Compiler warnings the change adds in the KMSDRM backend.
kmsdrm_warnings() {
    grep -oE 'src/video/kmsdrm/[A-Za-z_]+\.[ch]:[0-9]+:[0-9]+: warning: .*' "$1" | sort -u || true
}

step "3. backend: build this tree's SDL"
build_sdl "$SRC" head
kmsdrm_warnings "$WORK/build-head.build.log" >"$WORK/warnings-head.txt"
cat "$WORK/warnings-head.txt"
if grep -qE 'SDL_kmsdrm(rotate|orientation|rotategl)\.[ch]' "$WORK/warnings-head.txt"; then
    echo "FAIL: compiler warnings in the rotated-present sources" >&2
    exit 1
fi

step "3. backend: fake display stack"
# fake: the full stack. fake-nolink: the same stack without glLinkProgram.
build_fake() {
    local dir=$1
    shift
    mkdir -p "$dir"
    # shellcheck disable=SC2046
    cc "${STRICT[@]}" "$@" -fPIC -shared -Wl,-soname,libfake-kms.so $(pkg-config --cflags libdrm gbm) \
        -o "$dir/libfake-kms.so" "$T/fake-kms.c"
    for soname in libdrm.so.2 libgbm.so.1 libEGL.so.1 libGLESv2.so.2 libvulkan.so.1; do
        ln -sf libfake-kms.so "$dir/$soname"
    done
}
build_fake "$WORK/fake"
build_fake "$WORK/fake-nolink" -DFAKE_KMS_OMIT_GL_LINK_PROGRAM
# The app's own sources are held to -Werror; SDL's public headers are not ours.
# shellcheck disable=SC2046
cc -std=c99 -O1 -g -Wall -Wextra "${NOX11[@]}" $(pkg-config --cflags gbm) \
    -I"$WORK/prefix-head/include" -I"$T" -o "$WORK/test-kmsdrm-rotation" "$T/test-kmsdrm-rotation.c" \
    -L"$WORK/prefix-head/lib" -lSDL3-pocketforge -ldl

mkdir -p /dev/dri
: >/dev/dri/card0

failed=()
# name, SDL prefix, orientation, atomic, native fence, fence sync, opt-out hint, expected rotation, expected panel property
# (FAKE_DIR, EXPECT_WINDOW_FAIL, EXPECT_VULKAN_WINDOW, EXPECT_MAKECURRENT_FAIL and
# FAKE_EGL_FAIL_MAKECURRENT_SURFACE, when set by the caller, select the stack and the expected outcome)
run_case() {
    local name=$1 prefix=$2 orientation=$3 atomic=$4 native=$5 fences=$6 hint=$7 rotation=$8 panel=$9
    local log="$WORK/case-$name.log" rc=0
    env -u SDL_KMSDRM_PRESENT_ROTATION \
        LD_LIBRARY_PATH="${FAKE_DIR:-$WORK/fake}:$WORK/prefix-$prefix/lib" \
        EXPECT_WINDOW_FAIL="${EXPECT_WINDOW_FAIL:-0}" \
        EXPECT_VULKAN_WINDOW="${EXPECT_VULKAN_WINDOW:-0}" \
        EXPECT_MAKECURRENT_FAIL="${EXPECT_MAKECURRENT_FAIL:-0}" \
        FAKE_EGL_FAIL_MAKECURRENT_SURFACE="${FAKE_EGL_FAIL_MAKECURRENT_SURFACE:-}" \
        FAKE_KMS_PANEL_ORIENTATION="$orientation" FAKE_KMS_ATOMIC="$atomic" \
        FAKE_EGL_NATIVE_FENCE="$native" FAKE_EGL_FENCE_SYNC="$fences" \
        EXPECT_ROTATION="$rotation" EXPECT_PANEL_PROP="$panel" \
        ${hint:+SDL_KMSDRM_PRESENT_ROTATION=$hint} \
        "$WORK/test-kmsdrm-rotation" >"$log" 2>&1 || rc=$?
    printf '%-28s rc=%d  %s\n' "$name" "$rc" "$(grep -E '^RESULT:' "$log" || echo 'RESULT: none')"
    return "$rc"
}

step "3. backend: scenarios"
while read -r name orientation atomic native fences hint rotation panel; do
    [ -n "$name" ] || continue
    [ "$hint" = - ] && hint=
    orientation=${orientation//_/ }
    if ! run_case "$name" head "$orientation" "$atomic" "$native" "$fences" "$hint" "$rotation" "$panel"; then
        failed+=("$name")
        sed 's/^/    /' "$WORK/case-$name.log"
    fi
done <<'EOF'
lsu-fenced          Left_Side_Up   1 1 1 - 90  90
rsu-fenced          Right_Side_Up  1 1 1 - 270 270
normal-fenced       Normal         1 1 1 - 0   0
lsu-double          Left_Side_Up   1 0 1 - 90  90
rsu-double          Right_Side_Up  1 0 1 - 270 270
normal-double       Normal         1 0 1 - 0   0
lsu-legacy          Left_Side_Up   0 0 1 - 90  90
rsu-legacy          Right_Side_Up  0 0 1 - 270 270
normal-legacy       Normal         0 0 1 - 0   0
upside-down-fenced  Upside_Down    1 1 1 - 180 180
no-property-fenced  none           1 1 1 - 0   0
lsu-no-fence-sync   Left_Side_Up   1 0 0 - 90  90
lsu-opt-out         Left_Side_Up   1 1 1 0 0   90
EOF

# Vulkan windows stay panel-native: SDL rotates nothing for them, the display
# keeps the true panel orientation, and the window reports the application owns
# it (app_rotation). Normal is the negative control.
for spec in "lsu-vulkan-window|Left Side Up|90|90" "normal-vulkan-window|Normal|0|0"; do
    IFS='|' read -r name orientation rotation panel <<<"$spec"
    if ! EXPECT_VULKAN_WINDOW=1 run_case "$name" head "$orientation" 1 1 1 "" "$rotation" "$panel"; then
        failed+=("$name")
        sed 's/^/    /' "$WORK/case-$name.log"
    fi
done

# The final eglMakeCurrent of the frame-1 surface rebuild fails (gbm surface 2 is
# the rebuilt application surface: 0 and 1 are the first application and present
# surfaces). The swap fails and nothing the rebuild made may stay alive.
if ! FAKE_EGL_FAIL_MAKECURRENT_SURFACE=2 EXPECT_MAKECURRENT_FAIL=1 \
    run_case lsu-makecurrent-fails head "Left Side Up" 1 1 1 "" 90 90; then
    failed+=(lsu-makecurrent-fails)
    sed 's/^/    /' "$WORK/case-lsu-makecurrent-fails.log"
fi

# A GL entry point missing in the middle of the rotate pass's table: window
# creation fails cleanly (nothing called through the missing entry point, no
# surface, buffer, context or sync left behind).
if ! FAKE_DIR="$WORK/fake-nolink" EXPECT_WINDOW_FAIL=1 \
    run_case lsu-gl-load-fails head "Left Side Up" 1 1 1 "" 90 90; then
    failed+=(lsu-gl-load-fails)
    sed 's/^/    /' "$WORK/case-lsu-gl-load-fails.log"
fi

if [ "${#failed[@]}" -ne 0 ]; then
    echo "FAIL: backend scenarios: ${failed[*]}" >&2
    exit 1
fi

step "4. red: the base commit's SDL"
if [ -z "$BASE_SHA" ] || ! git -C "$SRC" cat-file -e "$BASE_SHA^{commit}" 2>/dev/null; then
    echo "SKIP: no base commit given (BASE_SHA)"
elif git -C "$SRC" cat-file -e "$BASE_SHA:sdl3/src/video/kmsdrm/SDL_kmsdrmrotate.c" 2>/dev/null; then
    echo "SKIP: base $BASE_SHA already has the rotated present"
else
    mkdir -p "$WORK/base-src"
    git -C "$SRC" archive "$BASE_SHA" sdl3 | tar -x -C "$WORK/base-src"
    build_sdl "$WORK/base-src" base
    kmsdrm_warnings "$WORK/build-base.build.log" >"$WORK/warnings-base.txt"
    # Warnings the change adds to upstream files it edits (line numbers move, so compare messages).
    added=$(comm -13 <(sed -E 's/:[0-9]+:[0-9]+:/:/' "$WORK/warnings-base.txt" | sort -u) \
                     <(sed -E 's/:[0-9]+:[0-9]+:/:/' "$WORK/warnings-head.txt" | sort -u))
    if [ -n "$added" ]; then
        echo "FAIL: the change adds KMSDRM compiler warnings:" >&2
        echo "$added" >&2
        exit 1
    fi
    if run_case base-lsu-fenced base "Left Side Up" 1 1 1 "" 90 90; then
        echo "FAIL: the base SDL passed a Left Side Up panel; the test does not detect the defect" >&2
        exit 1
    fi
    if ! grep -q 'FAIL: desktop mode 720x1280, expected 1280x720' "$WORK/case-base-lsu-fenced.log"; then
        echo "FAIL: the base SDL failed, but not with the portrait-size symptom:" >&2
        sed 's/^/    /' "$WORK/case-base-lsu-fenced.log" >&2
        exit 1
    fi
    echo "ok: the base SDL reports a 720x1280 display for a Left Side Up panel (the defect this change fixes)"
fi

LEAK_SHA=a71a8dabfa80987340dd4f9cf608bbbf70288968
step "5. red: the surface-rebuild cleanup before its fix ($LEAK_SHA)"
if ! git -C "$SRC" cat-file -e "$LEAK_SHA^{commit}" 2>/dev/null; then
    echo "SKIP: $LEAK_SHA is not in this checkout"
else
    mkdir -p "$WORK/leak-src"
    git -C "$SRC" archive "$LEAK_SHA" sdl3 | tar -x -C "$WORK/leak-src"
    build_sdl "$WORK/leak-src" leak
    if FAKE_EGL_FAIL_MAKECURRENT_SURFACE=2 EXPECT_MAKECURRENT_FAIL=1 \
        run_case leak-lsu-makecurrent-fails leak "Left Side Up" 1 1 1 "" 90 90; then
        echo "FAIL: $LEAK_SHA passed the MakeCurrent-failure scenario; the test does not detect the leak" >&2
        exit 1
    fi
    if ! grep -q 'FAIL: after the failed surface rebuild: nothing leaked' "$WORK/case-leak-lsu-makecurrent-fails.log"; then
        echo "FAIL: $LEAK_SHA failed the scenario, but not on the leak:" >&2
        sed 's/^/    /' "$WORK/case-leak-lsu-makecurrent-fails.log" >&2
        exit 1
    fi
    grep -E 'injected failures|FAIL: after the failed surface rebuild' "$WORK/case-leak-lsu-makecurrent-fails.log"
    echo "ok: $LEAK_SHA leaks the rebuild's surfaces and context when the final eglMakeCurrent fails (the defect fixed here)"
fi

step "RESULT: PASS"
