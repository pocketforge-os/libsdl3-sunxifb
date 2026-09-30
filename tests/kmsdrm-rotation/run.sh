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
#                pixel table and the panel corners; SDL_kmsdrmcommit.h, the
#                busy/failed classification of an atomic commit result;
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
#                The same app also runs every SDL_KMSDRM_ROTATE_EXPERIMENT pass
#                (tsp-mc9m.41.924.16.13.3): each must say it is on, keep the
#                present path working, and do what it names; an unknown name is
#                ignored, and without one no experiment entry point is touched;
#   6. red       with flips that take time to complete (FAKE_KMS_FLIP_MS), the
#                base commit's SDL must hit -EBUSY on a nonblocking commit,
#                because its wait for the previous flip is an EGL fence wait
#                that returns at once on Zink (tsp-mc9m.41.924.16.13; skipped
#                once the base waits on the OUT_FENCE itself).
#   7. red       the base commit's SDL must fail the twiddle experiment
#                scenario: it has no experiments, so no witness line and no
#                copy (tsp-mc9m.41.924.16.13.3; skipped once the base has them).
#   8. red       a pinned commit immediately before the RGB565 implementation
#                must fail the RGB565 rotated-source scenario: it ignores the
#                opt-in and keeps the app surface ARGB8888
#                (tsp-mc9m.41.924.16.13.3.1).
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

step "1. unit: SDL_Renderer pre-rotation transforms"
cc "${STRICT[@]}" -I"$KMSDRM" -o "$WORK/test-prerotate" "$T/test-prerotate.c"
"$WORK/test-prerotate"

step "1. unit: atomic commit result classification"
cc "${STRICT[@]}" -I"$KMSDRM" -o "$WORK/test-commit-classify" "$T/test-commit-classify.c"
"$WORK/test-commit-classify"

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
if grep -qE 'SDL_kmsdrm(rotate|orientation|rotategl|timing|commit)\.[ch]' "$WORK/warnings-head.txt"; then
    echo "FAIL: compiler warnings in the rotated-present or present-timing sources" >&2
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
cc -std=c99 -O1 -g -Wall -Wextra "${NOX11[@]}" \
    -I"$WORK/prefix-head/include" -I"$T" -I"$KMSDRM" \
    -o "$WORK/test-kmsdrm-prerotate" "$T/test-kmsdrm-prerotate.c" \
    -L"$WORK/prefix-head/lib" -lSDL3-pocketforge -ldl
cc -std=c99 -O1 -g -Wall -Wextra "${NOX11[@]}" \
    -I"$WORK/prefix-head/include" -I"$T" -I"$KMSDRM" \
    -o "$WORK/test-kmsdrm-cursor" "$T/test-kmsdrm-cursor.c" \
    -L"$WORK/prefix-head/lib" -lSDL3-pocketforge -ldl

mkdir -p /dev/dri
: >/dev/dri/card0

failed=()
# name, SDL prefix, orientation, atomic, native fence, fence sync, opt-out hint, expected rotation, expected panel property
# (FAKE_DIR, EXPECT_WINDOW_FAIL, EXPECT_VULKAN_WINDOW, EXPECT_MAKECURRENT_FAIL,
# FAKE_EGL_FAIL_MAKECURRENT_SURFACE, FAKE_KMS_FLIP_MS and SDL_KMSDRM_PRESENT_TIMING,
# when set by the caller, select the stack and the expected outcome)
run_case() {
    local name=$1 prefix=$2 orientation=$3 atomic=$4 native=$5 fences=$6 hint=$7 rotation=$8 panel=$9
    local log="$WORK/case-$name.log" rc=0
    env -u SDL_KMSDRM_PRESENT_ROTATION \
        LD_LIBRARY_PATH="${FAKE_DIR:-$WORK/fake}:$WORK/prefix-$prefix/lib" \
        EXPECT_WINDOW_FAIL="${EXPECT_WINDOW_FAIL:-0}" \
        EXPECT_FORMAT_FAIL="${EXPECT_FORMAT_FAIL:-0}" \
        EXPECT_SOURCE_RGB565="${EXPECT_SOURCE_RGB565:-0}" \
        EXPECT_VULKAN_WINDOW="${EXPECT_VULKAN_WINDOW:-0}" \
        EXPECT_MAKECURRENT_FAIL="${EXPECT_MAKECURRENT_FAIL:-0}" \
        EXPECT_SINGLE_FRAME="${EXPECT_SINGLE_FRAME:-0}" \
        EXPECT_CONTEXT_FAIL="${EXPECT_CONTEXT_FAIL:-0}" \
        FAKE_EGL_FAIL_MAKECURRENT_SURFACE="${FAKE_EGL_FAIL_MAKECURRENT_SURFACE:-}" \
        FAKE_KMS_FLIP_MS="${FAKE_KMS_FLIP_MS:-}" \
        SDL_KMSDRM_PRESENT_TIMING="${SDL_KMSDRM_PRESENT_TIMING:-}" \
        SDL_KMSDRM_ROTATE_EXPERIMENT="${SDL_KMSDRM_ROTATE_EXPERIMENT:-}" \
        SDL_KMSDRM_ROTATE_SOURCE_FORMAT="${SDL_KMSDRM_ROTATE_SOURCE_FORMAT:-}" \
        SDL_KMSDRM_RENDERER_PREROTATION="${SDL_KMSDRM_RENDERER_PREROTATION:-}" \
        EXPECT_EXPERIMENT="${EXPECT_EXPERIMENT:-}" \
        FAKE_GBM_RGB565="${FAKE_GBM_RGB565:-1}" \
        FAKE_EGL_RGB565="${FAKE_EGL_RGB565:-1}" \
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

# tsp-mc9m.41.924.16.13.3.1: a direct RGB565 application drawable halves the
# bytes sampled by the rotate pass. Positive 90/270 cases retain the same
# geometry checks as the ARGB scenarios above; the ordinary lsu-fenced case is
# the in-invocation default-format negative control. A Normal panel has no
# rotate source, so the opt-in must not change its direct scanout surface. An
# unsupported explicit request, or one without a matching EGLConfig, fails
# before allocation instead of silently benchmarking ARGB8888.
source_format_witness() {  # case
    local name=$1 lines
    lines=$(grep -cx 'KMSDRM rotated present source format: RGB565 (2 bytes/pixel)' "$WORK/case-$name.log" || true)
    if [ "$lines" -lt 1 ]; then
        echo "FAIL: $name: no RGB565 applied-format witness"
        return 1
    fi
    echo "ok: $name: $lines RGB565 applied-format witness line(s)"
}
for spec in "rgb565-lsu|Left Side Up|90" "rgb565-rsu|Right Side Up|270"; do
    IFS='|' read -r name orientation rotation <<<"$spec"
    if ! { SDL_KMSDRM_ROTATE_SOURCE_FORMAT=rgb565 EXPECT_SOURCE_RGB565=1 \
           run_case "$name" head "$orientation" 1 1 1 "" "$rotation" "$rotation" &&
           source_format_witness "$name"; }; then
        failed+=("$name")
        sed 's/^/    /' "$WORK/case-$name.log"
    fi
done
if ! SDL_KMSDRM_ROTATE_SOURCE_FORMAT=rgb565 \
    run_case rgb565-normal head Normal 1 1 1 "" 0 0; then
    failed+=(rgb565-normal)
    sed 's/^/    /' "$WORK/case-rgb565-normal.log"
fi
if ! FAKE_GBM_RGB565=0 SDL_KMSDRM_ROTATE_SOURCE_FORMAT=rgb565 EXPECT_FORMAT_FAIL=1 \
    run_case rgb565-unsupported head "Left Side Up" 1 1 1 "" 90 90; then
    failed+=(rgb565-unsupported)
    sed 's/^/    /' "$WORK/case-rgb565-unsupported.log"
fi
if ! FAKE_EGL_RGB565=0 SDL_KMSDRM_ROTATE_SOURCE_FORMAT=rgb565 EXPECT_FORMAT_FAIL=2 \
    run_case rgb565-no-egl-config head "Right Side Up" 1 1 1 "" 270 270; then
    failed+=(rgb565-no-egl-config)
    sed 's/^/    /' "$WORK/case-rgb565-no-egl-config.log"
fi
for name in lsu-fenced rgb565-normal rgb565-unsupported rgb565-no-egl-config; do
    if grep -q '^KMSDRM rotated present source format: RGB565 ' "$WORK/case-$name.log"; then
        echo "FAIL: $name: RGB565 applied witness on a default, unrotated, or refused path"
        failed+=("$name-rgb565-witness")
    fi
done

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

# tsp-mc9m.41.924.16.13: flips that complete 4 ms after their commit, as a real
# panel's do. SDL must wait for the previous flip before the next nonblocking
# commit, although a CPU wait on the imported EGL fence returns at once (Zink);
# the fake fails a commit that gets ahead with -EBUSY. SDL_KMSDRM_PRESENT_TIMING
# logs one summary line per present stage when the window is destroyed.
timing_lines() {
    grep -cE 'KMSDRM present timing: ' "$WORK/case-$1.log" || true
}
# commit_fail: the unrotated atomic path's frame-1 commit is the upstream empty
# request after the fullscreen switch (see test-kmsdrm-rotation.c), so it counts
# one failed commit; the rotated path rebuilds its surfaces without committing.
check_timing() {  # case level rotation commit_fail stage...
    local name=$1 level=$2 rotation=$3 fails=$4 log="$WORK/case-$1.log" stage
    shift 4
    if ! grep -qE "KMSDRM present timing: level=$level rotation=$rotation frames=[1-9][0-9]* commit_busy=0 commit_fail=$fails\$" "$log"; then
        echo "FAIL: $name: no timing summary with level=$level rotation=$rotation commit_busy=0 commit_fail=$fails"
        return 1
    fi
    for stage in "$@"; do
        if ! grep -qE "KMSDRM present timing: stage=$stage n=[1-9][0-9]* mean_ms=[0-9.]+ p50_ms=[0-9.]+ p90_ms=[0-9.]+ max_ms=[0-9.]+\$" "$log"; then
            echo "FAIL: $name: no timing line for stage $stage"
            return 1
        fi
    done
    echo "ok: $name: timing summary level=$level rotation=$rotation, stages $*"
}
no_stage() {  # case stage...
    local name=$1 stage
    shift
    for stage in "$@"; do
        if grep -qE "KMSDRM present timing: stage=$stage " "$WORK/case-$name.log"; then
            echo "FAIL: $name: stage $stage timed, but it does not apply here"
            return 1
        fi
    done
}
flip_case() {  # name orientation native timing rotation panel
    local name=$1 orientation=$2 native=$3 timing=$4 rotation=$5 panel=$6
    FAKE_KMS_FLIP_MS=4 SDL_KMSDRM_PRESENT_TIMING="$timing" \
        run_case "$name" head "$orientation" 1 "$native" 1 "" "$rotation" "$panel"
}
if ! { flip_case flip-lsu-fenced-t1 "Left Side Up" 1 1 90 90 &&
       check_timing flip-lsu-fenced-t1 1 90 0 frame app app_swap rotate present flip_wait end &&
       no_stage flip-lsu-fenced-t1 app_gpu rotate_gpu; }; then
    failed+=(flip-lsu-fenced-t1)
    sed 's/^/    /' "$WORK/case-flip-lsu-fenced-t1.log"
fi
if ! { flip_case flip-normal-fenced-t1 Normal 1 1 0 0 &&
       check_timing flip-normal-fenced-t1 1 0 1 frame app present flip_wait &&
       no_stage flip-normal-fenced-t1 app_swap app_gpu rotate rotate_gpu end; }; then
    failed+=(flip-normal-fenced-t1)
    sed 's/^/    /' "$WORK/case-flip-normal-fenced-t1.log"
fi
if ! { flip_case flip-rsu-fenced-t2 "Right Side Up" 1 2 270 270 &&
       check_timing flip-rsu-fenced-t2 2 270 0 frame app app_swap app_gpu rotate rotate_gpu present flip_wait end; }; then
    failed+=(flip-rsu-fenced-t2)
    sed 's/^/    /' "$WORK/case-flip-rsu-fenced-t2.log"
fi
if ! { flip_case flip-normal-fenced-t2 Normal 1 2 0 0 &&
       check_timing flip-normal-fenced-t2 2 0 1 frame app app_gpu present flip_wait; }; then
    failed+=(flip-normal-fenced-t2)
    sed 's/^/    /' "$WORK/case-flip-normal-fenced-t2.log"
fi
# The double-buffered path commits blocking; the kernel (and the fake) wait.
if ! flip_case flip-lsu-double "Left Side Up" 0 "" 90 90; then
    failed+=(flip-lsu-double)
    sed 's/^/    /' "$WORK/case-flip-lsu-double.log"
fi
# Timing off (unset) is the default: nothing is logged.
for name in lsu-fenced normal-fenced flip-lsu-double; do
    if [ "$(timing_lines "$name")" != 0 ]; then
        echo "FAIL: $name: timing lines without SDL_KMSDRM_PRESENT_TIMING"
        failed+=("$name-timing-off")
    fi
done

# tsp-mc9m.41.924.16.13.3: the SDL_KMSDRM_ROTATE_EXPERIMENT device arms. Each
# must print its witness line, keep every frame presenting through the ordinary
# swap path, and do what its name says (test-kmsdrm-rotation.c checks the
# draw, copy and clear counts). The llvmpipe step checks their pixels.
experiment_case() {  # name experiment orientation rotation [flip_ms timing]
    local name=$1 experiment=$2 orientation=$3 rotation=$4 flip=${5:-} timing=${6:-}
    SDL_KMSDRM_ROTATE_EXPERIMENT="$experiment" EXPECT_EXPERIMENT="$experiment" \
        FAKE_KMS_FLIP_MS="$flip" SDL_KMSDRM_PRESENT_TIMING="$timing" \
        run_case "$name" head "$orientation" 1 1 1 "" "$rotation" "$rotation"
}
# One witness line per rotate-pass setup (the frame-1 surface rebuild makes a
# second one), every one naming that experiment.
witness() {  # case experiment
    local name=$1 experiment=$2 lines named
    lines=$(grep -cE '^KMSDRM rotated present experiment: ' "$WORK/case-$name.log" || true)
    named=$(grep -cx "KMSDRM rotated present experiment: $experiment" "$WORK/case-$name.log" || true)
    if [ "$lines" -lt 1 ] || [ "$named" != "$lines" ]; then
        echo "FAIL: $name: expected only 'KMSDRM rotated present experiment: $experiment' witness lines, found $named of $lines"
        return 1
    fi
    echo "ok: $name: $lines witness line(s) for $experiment"
}
for spec in "exp-sample0-rsu|sample0|Right Side Up|270" "exp-clear-rsu|clear|Right Side Up|270" \
            "exp-loadclear-lsu|loadclear|Left Side Up|90" "exp-twiddle-rsu|twiddle|Right Side Up|270" \
            "exp-twiddle-lsu|twiddle|Left Side Up|90"; do
    IFS='|' read -r name experiment orientation rotation <<<"$spec"
    if ! { experiment_case "$name" "$experiment" "$orientation" "$rotation" && witness "$name" "$experiment"; }; then
        failed+=("$name")
        sed 's/^/    /' "$WORK/case-$name.log"
    fi
done
# The arms as the bench runs them: flips that take time, level-2 timing.
for experiment in sample0 clear loadclear twiddle; do
    name=exp-$experiment-flip-t2
    if ! { experiment_case "$name" "$experiment" "Right Side Up" 270 4 2 && witness "$name" "$experiment" &&
           check_timing "$name" 2 270 0 frame app app_swap app_gpu rotate rotate_gpu present flip_wait end; }; then
        failed+=("$name")
        sed 's/^/    /' "$WORK/case-$name.log"
    fi
done
# An unknown name is ignored (said once), and the ordinary pass runs untouched.
if ! { SDL_KMSDRM_ROTATE_EXPERIMENT=bogus run_case exp-unknown head "Right Side Up" 1 1 1 "" 270 270 &&
       grep -q "KMSDRM rotated present: ignoring unknown SDL_KMSDRM_ROTATE_EXPERIMENT 'bogus'" "$WORK/case-exp-unknown.log" &&
       ! grep -q '^KMSDRM rotated present experiment: ' "$WORK/case-exp-unknown.log"; }; then
    echo "FAIL: exp-unknown: an unknown experiment must be ignored with a warning and no witness"
    failed+=(exp-unknown)
    sed 's/^/    /' "$WORK/case-exp-unknown.log"
fi
# A panel that needs no rotation has no rotate pass, so no experiment applies.
if ! { SDL_KMSDRM_ROTATE_EXPERIMENT=twiddle run_case exp-twiddle-normal head Normal 1 1 1 "" 0 0 &&
       ! grep -q 'KMSDRM rotated present experiment' "$WORK/case-exp-twiddle-normal.log"; }; then
    echo "FAIL: exp-twiddle-normal: an experiment on a Normal panel must change nothing"
    failed+=(exp-twiddle-normal)
    sed 's/^/    /' "$WORK/case-exp-twiddle-normal.log"
fi
# Without the variable no witness appears anywhere.
for name in lsu-fenced rsu-fenced flip-rsu-fenced-t2; do
    if grep -q 'KMSDRM rotated present experiment' "$WORK/case-$name.log"; then
        echo "FAIL: $name: an experiment line without SDL_KMSDRM_ROTATE_EXPERIMENT"
        failed+=("$name-experiment-off")
    fi
done

# The Stage A handshake has 90/270-degree positives and two controls in this
# invocation: the same SDL_Renderer without the hint, and raw GLES with it.
while read -r name orientation rotation enabled; do
    log="$WORK/case-$name.log"
    rc=0
    env LD_LIBRARY_PATH="$WORK/fake:$WORK/prefix-head/lib" \
        FAKE_KMS_PANEL_ORIENTATION="${orientation//_/ }" FAKE_KMS_ATOMIC=1 \
        FAKE_EGL_NATIVE_FENCE=1 FAKE_EGL_FENCE_SYNC=1 FAKE_GBM_CURSOR=1 \
        EXPECT_ROTATION=$rotation EXPECT_PREROTATE=$enabled \
        "$WORK/test-kmsdrm-prerotate" >"$log" 2>&1 || rc=$?
    printf '%-28s rc=%d  %s\n' "$name" "$rc" "$(grep -E '^RESULT:' "$log" || echo 'RESULT: none')"
    if [ "$rc" -ne 0 ]; then
        failed+=("$name")
        sed 's/^/    /' "$log"
    fi
done <<'EOF'
prerotate-renderer-off Left_Side_Up 90 0
prerotate-renderer-lsu Left_Side_Up 90 1
prerotate-renderer-rsu Right_Side_Up 270 1
EOF
# Cursor bitmap, footprint and hotspot use the same panel transform as the
# direct-rendered primary plane. The Normal case is the byte-identical control.
while read -r name orientation rotation enabled; do
    log="$WORK/case-$name.log"
    rc=0
    env LD_LIBRARY_PATH="$WORK/fake:$WORK/prefix-head/lib" \
        FAKE_KMS_PANEL_ORIENTATION="${orientation//_/ }" FAKE_KMS_ATOMIC=1 \
        FAKE_EGL_NATIVE_FENCE=1 FAKE_EGL_FENCE_SYNC=1 FAKE_GBM_CURSOR=1 \
        EXPECT_ROTATION=$rotation EXPECT_PREROTATE=$enabled \
        "$WORK/test-kmsdrm-cursor" >"$log" 2>&1 || rc=$?
    printf '%-28s rc=%d  %s\n' "$name" "$rc" "$(grep -E '^RESULT:' "$log" || echo 'RESULT: none')"
    if [ "$rc" -ne 0 ]; then
        failed+=("$name")
        sed 's/^/    /' "$log"
    fi
done <<'EOF'
prerotate-cursor-normal Normal 0 1
prerotate-cursor-off Left_Side_Up 90 0
prerotate-cursor-lsu Left_Side_Up 90 1
prerotate-cursor-upside-down Upside_Down 180 1
prerotate-cursor-rsu Right_Side_Up 270 1
EOF
if ! SDL_KMSDRM_RENDERER_PREROTATION=1 \
    run_case prerotate-raw-fallback head "Left Side Up" 1 1 1 "" 90 90; then
    failed+=(prerotate-raw-fallback)
    sed 's/^/    /' "$WORK/case-prerotate-raw-fallback.log"
fi
if ! EXPECT_SINGLE_FRAME=1 SDL_KMSDRM_RENDERER_PREROTATION=1 \
    run_case prerotate-raw-one-frame head "Left Side Up" 1 1 1 "" 90 90; then
    failed+=(prerotate-raw-one-frame)
    sed 's/^/    /' "$WORK/case-prerotate-raw-one-frame.log"
fi
if ! FAKE_DIR="$WORK/fake-nolink" EXPECT_CONTEXT_FAIL=1 SDL_KMSDRM_RENDERER_PREROTATION=1 \
    run_case prerotate-raw-fallback-fails head "Left Side Up" 1 1 1 "" 90 90; then
    failed+=(prerotate-raw-fallback-fails)
    sed 's/^/    /' "$WORK/case-prerotate-raw-fallback-fails.log"
fi
# The round-3 pre-render fallback also cleans up if initializing the rotate
# context fails after both replacement surfaces have been allocated. Surface 2
# is the fallback's rotate target (surface 0 was the discarded candidate).
if ! FAKE_EGL_FAIL_MAKECURRENT_SURFACE=2 EXPECT_CONTEXT_FAIL=2 \
    SDL_KMSDRM_RENDERER_PREROTATION=1 \
    run_case prerotate-raw-makecurrent-fails head "Left Side Up" 1 1 1 "" 90 90; then
    failed+=(prerotate-raw-makecurrent-fails)
    sed 's/^/    /' "$WORK/case-prerotate-raw-makecurrent-fails.log"
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

step "6. red: the base commit's wait for the previous flip"
if [ -z "$BASE_SHA" ] || ! git -C "$SRC" cat-file -e "$BASE_SHA^{commit}" 2>/dev/null; then
    echo "SKIP: no base commit given (BASE_SHA)"
elif git -C "$SRC" grep -q kms_out_fence_wait_fd "$BASE_SHA" -- \
     sdl3/src/video/kmsdrm/SDL_kmsdrmvideo.h 2>/dev/null; then
    echo "SKIP: base $BASE_SHA already waits on the OUT_FENCE itself"
else
    if [ ! -d "$WORK/prefix-base" ]; then
        mkdir -p "$WORK/base-src"
        git -C "$SRC" archive "$BASE_SHA" sdl3 | tar -x -C "$WORK/base-src"
        build_sdl "$WORK/base-src" base
    fi
    for spec in "base-flip-lsu-fenced|Left Side Up|90" "base-flip-normal-fenced|Normal|0"; do
        IFS='|' read -r name orientation rotation <<<"$spec"
        if FAKE_KMS_FLIP_MS=4 run_case "$name" base "$orientation" 1 1 1 "" "$rotation" "$rotation"; then
            echo "FAIL: the base SDL passed $name; the test does not detect the defect" >&2
            exit 1
        fi
        if ! grep -q 'nonblocking atomic commit while the previous flip is still pending (-EBUSY)' "$WORK/case-$name.log"; then
            echo "FAIL: the base SDL failed $name, but not with -EBUSY:" >&2
            sed 's/^/    /' "$WORK/case-$name.log" >&2
            exit 1
        fi
        echo "ok: $name: the base SDL's next nonblocking commit reaches a pending flip (-EBUSY), the defect fixed here"
    done
fi

step "7. red: the base commit's SDL has no rotate-pass experiments"
if [ -z "$BASE_SHA" ] || ! git -C "$SRC" cat-file -e "$BASE_SHA^{commit}" 2>/dev/null; then
    echo "SKIP: no base commit given (BASE_SHA)"
elif git -C "$SRC" grep -q SDL_KMSDRM_ROTATE_EXPERIMENT "$BASE_SHA" -- \
     sdl3/src/video/kmsdrm/SDL_kmsdrmrotategl.h 2>/dev/null; then
    echo "SKIP: base $BASE_SHA already has the experiments"
else
    if [ ! -d "$WORK/prefix-base" ]; then
        mkdir -p "$WORK/base-src"
        git -C "$SRC" archive "$BASE_SHA" sdl3 | tar -x -C "$WORK/base-src"
        build_sdl "$WORK/base-src" base
    fi
    if SDL_KMSDRM_ROTATE_EXPERIMENT=twiddle EXPECT_EXPERIMENT=twiddle \
        run_case base-exp-twiddle-rsu base "Right Side Up" 1 1 1 "" 270 270 && witness base-exp-twiddle-rsu twiddle; then
        echo "FAIL: the base SDL passed the twiddle experiment scenario; the test does not detect a missing experiment" >&2
        exit 1
    fi
    if ! grep -qE 'FAIL: twiddle experiment: every pass samples a copy of the frame \(0 copies' "$WORK/case-base-exp-twiddle-rsu.log"; then
        echo "FAIL: the base SDL failed the twiddle scenario, but not for want of the copy:" >&2
        sed 's/^/    /' "$WORK/case-base-exp-twiddle-rsu.log" >&2
        exit 1
    fi
    echo "ok: the base SDL ignores SDL_KMSDRM_ROTATE_EXPERIMENT=twiddle (no copy, no witness): the arm needs this change"
fi

RGB565_MISSING_SHA=2c63b318f0d1b1f9eeb82b29bb14856d38d19bd9
step "8. red: RGB565 rotate-source opt-in before its fix ($RGB565_MISSING_SHA)"
if ! git -C "$SRC" cat-file -e "$RGB565_MISSING_SHA^{commit}" 2>/dev/null; then
    echo "SKIP: $RGB565_MISSING_SHA is not in this checkout"
else
    mkdir -p "$WORK/rgb565-missing-src"
    git -C "$SRC" archive "$RGB565_MISSING_SHA" sdl3 | tar -x -C "$WORK/rgb565-missing-src"
    build_sdl "$WORK/rgb565-missing-src" rgb565-missing
    if SDL_KMSDRM_ROTATE_SOURCE_FORMAT=rgb565 EXPECT_SOURCE_RGB565=1 \
        run_case missing-rgb565-rsu rgb565-missing "Right Side Up" 1 1 1 "" 270 270; then
        echo "FAIL: $RGB565_MISSING_SHA passed the RGB565 source scenario; the test does not detect the missing opt-in" >&2
        exit 1
    fi
    if ! grep -q 'FAIL: the application surface format is RGB565' "$WORK/case-missing-rgb565-rsu.log"; then
        echo "FAIL: $RGB565_MISSING_SHA failed the RGB565 scenario, but not on its ARGB8888 source:" >&2
        sed 's/^/    /' "$WORK/case-missing-rgb565-rsu.log" >&2
        exit 1
    fi
    echo "ok: $RGB565_MISSING_SHA keeps the rotated application source ARGB8888, the defect fixed by the RGB565 opt-in"
fi

step "RESULT: PASS"
