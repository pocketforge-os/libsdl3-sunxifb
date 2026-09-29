#!/bin/sh
# Cheap source and controlled-fixture contract for invariants that normally run
# in the pinned cross-build container.
# Literal shell fragments below are assertions or generated test stubs.
# shellcheck disable=SC2016
set -eu

BUILD_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
SCRIPT=${BUILD_DIR}/build-libsdl3.sh
VERIFY=${BUILD_DIR}/verify-libsdl3-artifact.sh
DOCKERFILE=${BUILD_DIR}/Dockerfile
TOOLCHAIN=${BUILD_DIR}/toolchain-arm-10.3-2021.07.cmake

require_literal() {
  file=$1
  literal=$2
  if ! grep -Fq -- "${literal}" "${file}"; then
    echo "FAIL: ${file} is missing: ${literal}"
    exit 1
  fi
}

require_literal "${SCRIPT}" '-DSDL_RPATH=OFF'
require_literal "${SCRIPT}" '-DCMAKE_SKIP_RPATH=ON'
require_literal "${SCRIPT}" '-DSDL_KMSDRM="${SDL_KMSDRM}"'
require_literal "${SCRIPT}" '-DSDL_KMSDRM_SHARED=ON'
require_literal "${SCRIPT}" '-DSDL_SUNXIFB="${SDL_SUNXIFB}"'
require_literal "${SCRIPT}" 'require_target_pkg_config gbm'
require_literal "${SCRIPT}" 'GRAPHICS_CONFIG_STAMP=${OUT}/.pocketforge-sdl-graphics-config'
require_literal "${SCRIPT}" 'rm -f "${OUT}/CMakeCache.txt"'
require_literal "${SCRIPT}" 'verify-libsdl3-artifact.sh'
require_literal "${DOCKERFILE}" 'libgbm-dev:arm64'
require_literal "${DOCKERFILE}" "'libgbm*'"
require_literal "${DOCKERFILE}" 'pkg-config --exists alsa libdrm gbm'
require_literal "${DOCKERFILE}" 'test -f "${DST_INC}/gbm.h"'
require_literal "${DOCKERFILE}" 'test -f "${DST_INC}/xf86drm.h"'
require_literal "${DOCKERFILE}" 'test -f "${DST_INC}/xf86drmMode.h"'
require_literal "${TOOLCHAIN}" 'list(APPEND CMAKE_FIND_ROOT_PATH "${SUNXIFB_DDK_ROOT}")'

TMP=$(mktemp -d)
trap 'rm -rf "${TMP}"' EXIT HUP INT TERM
SO=${TMP}/libSDL3-pocketforge.so.0.5.0
: >"${SO}"

make_stub() {
  name=$1
  shift
  {
    echo '#!/bin/sh'
    printf '%s\n' "$@"
  } >"${TMP}/${name}"
  chmod +x "${TMP}/${name}"
}

# nm stubs answer `nm -D` (dynamic symbols) and plain `nm` (the full symbol
# table, which names the compiled video bootstraps).
make_nm_stub() {
  name=$1
  shift
  make_stub "${name}" \
    'if [ "${1:-}" = -D ]; then echo "00000000 T SDL_DYNAPI_entry"; exit 0; fi' \
    'echo "00000000 T SDL_DYNAPI_entry"' \
    "$@"
}

# Current (pre-tsp-f3fm.218) open build shape: sunxifb and KMSDRM together.
make_stub readelf-ok \
  "printf '%s\\n' ' 0x1 (NEEDED) Shared library: [libEGL.so.1]' ' 0x1 (NEEDED) Shared library: [libGLESv2.so.2]'"
make_stub readelf-no-gl "echo ' 0x1 (NEEDED) Shared library: [libc.so.6]'"
make_nm_stub nm-ddk 'echo "00001000 d SUNXIFB_bootstrap"'
make_nm_stub nm-open 'echo "00002000 d KMSDRM_bootstrap"'
make_nm_stub nm-open-with-sunxifb 'echo "00001000 d SUNXIFB_bootstrap"' 'echo "00002000 d KMSDRM_bootstrap"'
make_nm_stub nm-ddk-with-kms 'echo "00001000 d SUNXIFB_bootstrap"' 'echo "00002000 d KMSDRM_bootstrap"'
make_stub nm-stripped \
  'if [ "${1:-}" = -D ]; then echo "00000000 T SDL_DYNAPI_entry"; exit 0; fi' \
  'echo "nm: $1: no symbols" >&2'
make_stub nm-symtab-fails \
  'if [ "${1:-}" = -D ]; then echo "00000000 T SDL_DYNAPI_entry"; exit 0; fi' \
  'exit 23'
make_stub strings-ddk "printf '%s\\n' sunxifb dummy"
make_stub strings-open "printf '%s\\n' kmsdrm dummy libdrm.so.2 libgbm.so.1"
make_stub strings-open-with-sunxifb "printf '%s\\n' sunxifb kmsdrm dummy libdrm.so.2 libgbm.so.1"
make_stub strings-open-no-kms "printf '%s\\n' dummy libdrm.so.2 libgbm.so.1"
make_stub strings-open-no-gbm "printf '%s\\n' kmsdrm dummy libdrm.so.2"
make_stub strings-ddk-with-kms "printf '%s\\n' sunxifb kmsdrm dummy"
make_stub strings-no-sunxifb "printf '%s\\n' dummy"
make_stub symver-ok 'exit 0'
make_stub fail 'exit 23'

run_verify() {
  model=$1
  readelf=$2
  nm=$3
  strings=$4
  symver=$5
  PF_GPU_MODEL=${model} READELF=${readelf} NM=${nm} STRINGS=${strings} \
    SYMVER_CHECK=${symver} "${VERIFY}" "${SO}"
}

# expect_verify_fail <reason> <expected stderr> <run_verify args...>
expect_verify_fail() {
  reason=$1
  expected=$2
  shift 2
  if run_verify "$@" >"${TMP}/out" 2>"${TMP}/err"; then
    echo "FAIL: verification accepted ${reason}"
    exit 1
  fi
  if ! grep -Fq -- "${expected}" "${TMP}/err"; then
    echo "FAIL: verification rejected ${reason}, but not with: ${expected}"
    sed 's/^/    /' "${TMP}/err"
    exit 1
  fi
}

expect_verify_fail 'a failing readelf' 'FATAL: readelf inspection failed' \
  ddk "${TMP}/fail" "${TMP}/nm-ddk" "${TMP}/strings-ddk" "${TMP}/symver-ok"
expect_verify_fail 'a failing strings tool' 'FATAL: strings inspection failed' \
  ddk "${TMP}/readelf-ok" "${TMP}/nm-ddk" "${TMP}/fail" "${TMP}/symver-ok"
expect_verify_fail 'a failing nm' 'FATAL: nm inspection failed' \
  ddk "${TMP}/readelf-ok" "${TMP}/fail" "${TMP}/strings-ddk" "${TMP}/symver-ok"
expect_verify_fail 'a failing symbol-table nm' 'FATAL: nm symbol-table inspection failed' \
  ddk "${TMP}/readelf-ok" "${TMP}/nm-symtab-fails" "${TMP}/strings-ddk" "${TMP}/symver-ok"
expect_verify_fail 'a failing symbol-version check' 'FATAL: glibc symbol-version inspection failed' \
  ddk "${TMP}/readelf-ok" "${TMP}/nm-ddk" "${TMP}/strings-ddk" "${TMP}/fail"

make_stub readelf-rpath \
  "printf '%s\\n' ' 0x1 (NEEDED) Shared library: [libEGL.so.1]' ' 0x1 (NEEDED) Shared library: [libGLESv2.so.2]' ' 0x0 (RUNPATH) Library runpath: [/tmp]'"
expect_verify_fail 'RUNPATH' 'FATAL: shipped SDL artifact contains RPATH or RUNPATH' \
  ddk "${TMP}/readelf-rpath" "${TMP}/nm-ddk" "${TMP}/strings-ddk" "${TMP}/symver-ok"
expect_verify_fail 'RUNPATH on an open artifact' 'FATAL: shipped SDL artifact contains RPATH or RUNPATH' \
  open "${TMP}/readelf-rpath" "${TMP}/nm-open" "${TMP}/strings-open" "${TMP}/symver-ok"

make_stub readelf-no-egl "echo ' 0x1 (NEEDED) Shared library: [libGLESv2.so.2]'"
expect_verify_fail 'a closed artifact without libEGL' 'FATAL: SDL artifact is missing required dependency libEGL.so.1' \
  ddk "${TMP}/readelf-no-egl" "${TMP}/nm-ddk" "${TMP}/strings-ddk" "${TMP}/symver-ok"

# --- closed (ddk) model: sunxifb present, KMSDRM absent ----------------------
run_verify ddk "${TMP}/readelf-ok" "${TMP}/nm-ddk" "${TMP}/strings-ddk" "${TMP}/symver-ok" \
  >"${TMP}/out" 2>"${TMP}/err"
grep -Fxq 'sunxifb' "${TMP}/out"
grep -Fxq 'SUNXIFB_bootstrap' "${TMP}/out"

expect_verify_fail 'a closed artifact with the KMSDRM string' \
  'FATAL: closed SDL artifact unexpectedly contains the KMSDRM backend' \
  ddk "${TMP}/readelf-ok" "${TMP}/nm-ddk" "${TMP}/strings-ddk-with-kms" "${TMP}/symver-ok"
expect_verify_fail 'a closed artifact with the KMSDRM bootstrap symbol' \
  'FATAL: closed SDL artifact unexpectedly contains the KMSDRM backend' \
  ddk "${TMP}/readelf-ok" "${TMP}/nm-ddk-with-kms" "${TMP}/strings-ddk" "${TMP}/symver-ok"
expect_verify_fail 'a closed artifact without the sunxifb backend' \
  'FATAL: closed SDL artifact is missing the sunxifb backend' \
  ddk "${TMP}/readelf-ok" "${TMP}/nm-ddk" "${TMP}/strings-no-sunxifb" "${TMP}/symver-ok"
expect_verify_fail 'a closed artifact without a symbol table' \
  'FATAL: closed SDL artifact is missing the SUNXIFB_bootstrap symbol' \
  ddk "${TMP}/readelf-ok" "${TMP}/nm-stripped" "${TMP}/strings-ddk" "${TMP}/symver-ok"

# --- open model: KMSDRM present, sunxifb absent (tsp-f3fm.218) ---------------
# An open artifact needs no direct libEGL/libGLESv2: only sunxifb links them.
run_verify open "${TMP}/readelf-no-gl" "${TMP}/nm-open" "${TMP}/strings-open" "${TMP}/symver-ok" \
  >"${TMP}/out" 2>"${TMP}/err"
grep -Fxq 'kmsdrm' "${TMP}/out"
grep -Fxq 'KMSDRM_bootstrap' "${TMP}/out"
if grep -Fxq 'sunxifb' "${TMP}/out" || grep -Fxq 'SUNXIFB_bootstrap' "${TMP}/out"; then
  echo "FAIL: open verification listed a sunxifb backend it should not contain"
  exit 1
fi

# The artifact the pre-tsp-f3fm.218 recipe produced for the open model.
expect_verify_fail 'an open artifact carrying sunxifb (the pre-tsp-f3fm.218 open build)' \
  'FATAL: open SDL artifact contains the sunxifb backend' \
  open "${TMP}/readelf-ok" "${TMP}/nm-open-with-sunxifb" "${TMP}/strings-open-with-sunxifb" "${TMP}/symver-ok"
expect_verify_fail 'an open artifact with only the SUNXIFB bootstrap symbol' \
  'FATAL: open SDL artifact contains the SUNXIFB_bootstrap symbol' \
  open "${TMP}/readelf-ok" "${TMP}/nm-open-with-sunxifb" "${TMP}/strings-open" "${TMP}/symver-ok"
expect_verify_fail 'an open artifact without a symbol table' \
  'FATAL: open SDL artifact is missing the KMSDRM_bootstrap symbol' \
  open "${TMP}/readelf-ok" "${TMP}/nm-stripped" "${TMP}/strings-open" "${TMP}/symver-ok"
expect_verify_fail 'an open artifact without the KMSDRM backend' \
  'FATAL: open SDL artifact is missing compiled KMSDRM evidence: kmsdrm' \
  open "${TMP}/readelf-ok" "${TMP}/nm-open" "${TMP}/strings-open-no-kms" "${TMP}/symver-ok"
expect_verify_fail 'non-dynamic KMSDRM evidence' \
  'FATAL: open SDL artifact is missing compiled KMSDRM evidence: libgbm.so.1' \
  open "${TMP}/readelf-ok" "${TMP}/nm-open" "${TMP}/strings-open-no-gbm" "${TMP}/symver-ok"
expect_verify_fail 'an invalid GPU model' "FATAL: PF_GPU_MODEL must be 'ddk' or 'open'" \
  bogus "${TMP}/readelf-ok" "${TMP}/nm-ddk" "${TMP}/strings-ddk" "${TMP}/symver-ok"

# --- real ELF controls --------------------------------------------------------
# Host-compiled shared objects inspected with the real readelf, nm and strings,
# so the verifier's parsing of genuine tool output is exercised, not only the
# stubs' output. Each C fixture defines the bootstrap symbols and driver-name
# strings an SDL build of that shape would carry.
HOST_CC=${CC:-cc}
for tool in "${HOST_CC}" readelf nm strings strip; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "FAIL: real ELF controls require ${tool}"
    exit 1
  fi
done
printf '%s\n' 'void fixture_dependency(void) {}' >"${TMP}/dependency.c"
for soname in libEGL.so.1 libGLESv2.so.2 libdrm.so.2 libgbm.so.1; do
  "${HOST_CC}" -shared -fPIC -Wl,-soname,"${soname}" \
    -o "${TMP}/${soname}" "${TMP}/dependency.c"
done
ln -s libEGL.so.1 "${TMP}/libEGL.so"
ln -s libGLESv2.so.2 "${TMP}/libGLESv2.so"
ln -s libdrm.so.2 "${TMP}/libdrm.so"
ln -s libgbm.so.1 "${TMP}/libgbm.so"

# real_elf <name> <link args> -- <C lines...>
real_elf() {
  name=$1
  links=$2
  shift 3
  printf '%s\n' 'void SDL_DYNAPI_entry(void) {}' "$@" >"${TMP}/${name}.c"
  # shellcheck disable=SC2086
  "${HOST_CC}" -shared -fPIC -Wl,--no-as-needed -L"${TMP}" \
    -o "${TMP}/${name}.so" "${TMP}/${name}.c" ${links}
}

verify_real() {
  model=$1
  so=$2
  PF_GPU_MODEL=${model} READELF=readelf NM=nm STRINGS=strings \
    SYMVER_CHECK=${TMP}/symver-ok "${VERIFY}" "${so}"
}

expect_real_fail() {
  reason=$1
  expected=$2
  model=$3
  so=$4
  if verify_real "${model}" "${so}" >"${TMP}/out" 2>"${TMP}/err"; then
    echo "FAIL: ${model} verification accepted ${reason}"
    exit 1
  fi
  if ! grep -Fq -- "${expected}" "${TMP}/err"; then
    echo "FAIL: ${model} verification rejected ${reason}, but not with: ${expected}"
    sed 's/^/    /' "${TMP}/err"
    exit 1
  fi
}

KMSDRM_LINES='const char KMSDRM_bootstrap[] = "kmsdrm";
const char dynamic_libdrm[] = "libdrm.so.2";
const char dynamic_libgbm[] = "libgbm.so.1";'
SUNXIFB_LINES='const char SUNXIFB_bootstrap[] = "sunxifb";'

# Open build after tsp-f3fm.218: KMSDRM loaded dynamically, no sunxifb, no GL link.
real_elf real-open '' -- "${KMSDRM_LINES}"
verify_real open "${TMP}/real-open.so" >"${TMP}/out" 2>"${TMP}/err"
grep -Fxq 'KMSDRM_bootstrap' "${TMP}/out"

# Open build before tsp-f3fm.218: sunxifb (linking EGL/GLES) beside KMSDRM.
real_elf real-open-with-sunxifb '-lEGL -lGLESv2' -- "${KMSDRM_LINES}" "${SUNXIFB_LINES}"
expect_real_fail 'the pre-tsp-f3fm.218 open build shape' \
  'FATAL: open SDL artifact contains the sunxifb backend' \
  open "${TMP}/real-open-with-sunxifb.so"

# Closed build: sunxifb linking EGL/GLES, no KMSDRM.
real_elf real-ddk '-lEGL -lGLESv2' -- "${SUNXIFB_LINES}"
verify_real ddk "${TMP}/real-ddk.so" >"${TMP}/out" 2>"${TMP}/err"
grep -Fxq 'SUNXIFB_bootstrap' "${TMP}/out"
expect_real_fail 'a closed artifact without EGL/GLES links' \
  'FATAL: SDL artifact is missing required dependency libEGL.so.1' \
  ddk "${TMP}/real-open.so"

# A stripped closed artifact still carries the driver-name string but no symbol
# table; the bootstrap-symbol check must fail closed rather than pass.
cp "${TMP}/real-ddk.so" "${TMP}/real-ddk-stripped.so"
strip --strip-all "${TMP}/real-ddk-stripped.so"
expect_real_fail 'a stripped closed artifact' \
  'FATAL: closed SDL artifact is missing the SUNXIFB_bootstrap symbol' \
  ddk "${TMP}/real-ddk-stripped.so"

# A real ELF whose dynamic table directly links every graphics dependency while
# its symbols and strings otherwise resemble an acceptable open artifact. This
# must fail specifically on libdrm/libgbm DT_NEEDED rather than on a mocked
# readelf response.
real_elf direct-kms '-lEGL -lGLESv2 -ldrm -lgbm' -- "${KMSDRM_LINES}"
expect_real_fail 'directly linked KMSDRM dependencies' \
  'FATAL: open SDL artifact links libdrm.so directly instead of using dynamic KMSDRM loading' \
  open "${TMP}/direct-kms.so"

# --- canonical build entry point ----------------------------------------------
# Exercise build-libsdl3.sh with controlled tools. This proves model selection
# reaches both CMake and the real artifact verifier while also supplying
# negative controls for missing/host target metadata.
make_stub cmake \
  'if [ "${1:-}" = --build ]; then' \
  '  mkdir -p "${OUT}"' \
  '  : >"${OUT}/libSDL3-pocketforge.so.0.5.0"' \
  'else' \
  '  selected_root=' \
  '  for arg do' \
  '    case "${arg}" in -DSUNXIFB_DDK_ROOT=*) selected_root=${arg#*=} ;; esac' \
  '  done' \
  '  cached_root=' \
  '  if [ -f "${OUT}/CMakeCache.txt" ]; then IFS= read -r cached_root <"${OUT}/CMakeCache.txt" || true; fi' \
  '  if [ -n "${cached_root}" ] && [ "${cached_root}" != "${selected_root}" ]; then' \
  '    echo "FATAL: controlled CMake reused stale graphics root ${cached_root}" >&2' \
  '    exit 86' \
  '  fi' \
  '  printf "%s\\n" "${selected_root}" >"${OUT}/CMakeCache.txt"' \
  '  mkdir -p "${OUT}/CMakeFiles"' \
  '  printf "%s\\n" "$*" >>"${CMAKE_LOG}"' \
  'fi'
make_stub nproc 'echo 2'
make_stub pkg-config \
  'target_sysroot=/opt/arm-10.3-2021.07/aarch64-none-linux-gnu/libc' \
  'case "${1:-}" in' \
  '  --exists)' \
  '    shift' \
  '    for module do' \
  '      if [ "${PKG_CONFIG_STUB_MODE:-target}" = missing-gbm ] && [ "${module}" = gbm ]; then exit 1; fi' \
  '    done' \
  '    exit 0' \
  '    ;;' \
  '  --variable=pcfiledir)' \
  '    if [ "${PKG_CONFIG_STUB_MODE:-target}" = host ]; then echo /usr/lib/x86_64-linux-gnu/pkgconfig; else echo "${target_sysroot}/usr/lib/pkgconfig"; fi' \
  '    ;;' \
  '  --variable=libdir)' \
  '    if [ "${PKG_CONFIG_STUB_MODE:-target}" = host ]; then echo /usr/lib/x86_64-linux-gnu; else echo "${target_sysroot}/usr/lib"; fi' \
  '    ;;' \
  '  --variable=includedir)' \
  '    if [ "${PKG_CONFIG_STUB_MODE:-target}" = host ]; then echo /usr/include; else echo "${target_sysroot}/usr/include"; fi' \
  '    ;;' \
  '  *) exit 2 ;;' \
  'esac'
# The controlled artifact's shape follows ARTIFACT_KIND (default: the model).
make_stub strings-build \
  'case "${ARTIFACT_KIND:-${PF_GPU_MODEL:-ddk}}" in' \
  '  open) printf "%s\\n" kmsdrm dummy libdrm.so.2 libgbm.so.1 ;;' \
  '  open-with-sunxifb) printf "%s\\n" sunxifb kmsdrm dummy libdrm.so.2 libgbm.so.1 ;;' \
  '  open-missing-kms) printf "%s\\n" dummy libdrm.so.2 libgbm.so.1 ;;' \
  '  ddk) printf "%s\\n" sunxifb dummy ;;' \
  '  *) exit 2 ;;' \
  'esac'
make_stub nm-build \
  'if [ "${1:-}" = -D ]; then echo "00000000 T SDL_DYNAPI_entry"; exit 0; fi' \
  'case "${ARTIFACT_KIND:-${PF_GPU_MODEL:-ddk}}" in' \
  '  open) echo "00002000 d KMSDRM_bootstrap" ;;' \
  '  open-with-sunxifb) printf "%s\\n" "00001000 d SUNXIFB_bootstrap" "00002000 d KMSDRM_bootstrap" ;;' \
  '  open-missing-kms) echo "00000000 T SDL_DYNAPI_entry" ;;' \
  '  ddk) echo "00001000 d SUNXIFB_bootstrap" ;;' \
  '  *) exit 2 ;;' \
  'esac'
make_stub readelf-build \
  'case "${ARTIFACT_KIND:-${PF_GPU_MODEL:-ddk}}" in' \
  '  ddk|open-with-sunxifb) printf "%s\\n" " 0x1 (NEEDED) Shared library: [libEGL.so.1]" " 0x1 (NEEDED) Shared library: [libGLESv2.so.2]" ;;' \
  '  *) echo " 0x1 (NEEDED) Shared library: [libc.so.6]" ;;' \
  'esac'

run_build() {
  model=$1
  artifact_kind=$2
  pkg_mode=$3
  name=${model}-${artifact_kind}-${pkg_mode}
  out=${4:-${TMP}/out-${name}}
  log=${5:-${TMP}/cmake-${name}.log}
  mesa_root=${6:-${TMP}/mesa}
  blobs_root=${7:-${TMP}/blobs}
  mkdir -p "${out}"
  : >"${log}"
  PATH=${TMP}:${PATH} \
    CMAKE_LOG=${log} \
    ARTIFACT_KIND=${artifact_kind} \
    PKG_CONFIG_STUB_MODE=${pkg_mode} \
    READELF=${TMP}/readelf-build \
    NM=${TMP}/nm-build \
    STRINGS=${TMP}/strings-build \
    SYMVER_CHECK=${TMP}/symver-ok \
    OUT=${out} \
    SRC=${TMP}/source \
    BLOBS=${blobs_root} \
    PF_GPU_MODEL=${model} \
    SUNXIFB_MESA_ROOT=${mesa_root} \
    "${SCRIPT}"
}

require_cmake_arg() {
  log=$1
  arg=$2
  if ! grep -Fq -- "${arg}" "${log}"; then
    echo "FAIL: build-libsdl3.sh did not configure CMake with ${arg} ($(basename "${log}"))"
    exit 1
  fi
}

forbid_cmake_arg() {
  log=$1
  arg=$2
  if grep -Fq -- "${arg}" "${log}"; then
    echo "FAIL: build-libsdl3.sh configured CMake with ${arg} ($(basename "${log}"))"
    exit 1
  fi
}

run_build open open target >"${TMP}/build-out" 2>"${TMP}/build-err"
require_cmake_arg "${TMP}/cmake-open-open-target.log" '-DSDL_KMSDRM=ON'
require_cmake_arg "${TMP}/cmake-open-open-target.log" '-DSDL_KMSDRM_SHARED=ON'
# tsp-f3fm.218: the open model compiles no sunxifb.
require_cmake_arg "${TMP}/cmake-open-open-target.log" '-DSDL_SUNXIFB=OFF'
forbid_cmake_arg "${TMP}/cmake-open-open-target.log" '-DSDL_SUNXIFB=ON'
grep -Fxq 'kmsdrm' "${TMP}/build-out"
grep -Fxq 'KMSDRM_bootstrap' "${TMP}/build-out"
if grep -Fxq 'sunxifb' "${TMP}/build-out"; then
  echo "FAIL: open build reported a sunxifb backend"
  exit 1
fi

run_build ddk ddk target >"${TMP}/build-out" 2>"${TMP}/build-err"
require_cmake_arg "${TMP}/cmake-ddk-ddk-target.log" '-DSDL_KMSDRM=OFF'
require_cmake_arg "${TMP}/cmake-ddk-ddk-target.log" '-DSDL_SUNXIFB=ON'
forbid_cmake_arg "${TMP}/cmake-ddk-ddk-target.log" '-DSDL_SUNXIFB=OFF'
grep -Fxq 'sunxifb' "${TMP}/build-out"
grep -Fxq 'SUNXIFB_bootstrap' "${TMP}/build-out"

# The canonical entry point runs the real verifier: an open artifact that still
# carries sunxifb fails the build.
if run_build open open-with-sunxifb target >"${TMP}/build-out" 2>"${TMP}/build-err"; then
  echo "FAIL: open build accepted an artifact carrying sunxifb"
  exit 1
fi
grep -Fq 'FATAL: open SDL artifact contains the sunxifb backend' "${TMP}/build-err"

# Reuse one actual OUT through both model transitions. The controlled CMake
# stub fails if a stale cache survives, and the stamp assertions prove the
# canonical entry point records the new model/root before each configuration.
transition_out=${TMP}/out-transition
transition_log=${TMP}/cmake-transition.log
run_build ddk ddk target "${transition_out}" "${transition_log}" \
  "${TMP}/mesa-a" "${TMP}/ddk-a" >"${TMP}/build-out" 2>"${TMP}/build-err"
grep -Fxq 'PF_GPU_MODEL=ddk' "${transition_out}/.pocketforge-sdl-graphics-config"
grep -Fxq "SUNXIFB_DDK_ROOT=${TMP}/ddk-a" "${transition_out}/.pocketforge-sdl-graphics-config"
run_build open open target "${transition_out}" "${transition_log}" \
  "${TMP}/mesa-b" "${TMP}/ddk-a" >"${TMP}/build-out" 2>"${TMP}/build-err"
grep -Fxq 'PF_GPU_MODEL=open' "${transition_out}/.pocketforge-sdl-graphics-config"
grep -Fxq "SUNXIFB_DDK_ROOT=${TMP}/mesa-b" "${transition_out}/.pocketforge-sdl-graphics-config"
run_build ddk ddk target "${transition_out}" "${transition_log}" \
  "${TMP}/mesa-b" "${TMP}/ddk-c" >"${TMP}/build-out" 2>"${TMP}/build-err"
grep -Fxq 'PF_GPU_MODEL=ddk' "${transition_out}/.pocketforge-sdl-graphics-config"
grep -Fxq "SUNXIFB_DDK_ROOT=${TMP}/ddk-c" "${transition_out}/.pocketforge-sdl-graphics-config"
run_build ddk ddk target "${transition_out}" "${transition_log}" \
  "${TMP}/mesa-b" "${TMP}/ddk-d" >"${TMP}/build-out" 2>"${TMP}/build-err"
grep -Fxq 'PF_GPU_MODEL=ddk' "${transition_out}/.pocketforge-sdl-graphics-config"
grep -Fxq "SUNXIFB_DDK_ROOT=${TMP}/ddk-d" "${transition_out}/.pocketforge-sdl-graphics-config"

if run_build open open missing-gbm >"${TMP}/build-out" 2>"${TMP}/build-err"; then
  echo "FAIL: open build accepted missing target GBM metadata"
  exit 1
fi
grep -Fq "FATAL: target pkg-config module 'gbm' is required" "${TMP}/build-err"

if run_build open open host >"${TMP}/build-out" 2>"${TMP}/build-err"; then
  echo "FAIL: open build accepted host-architecture pkg-config metadata"
  exit 1
fi
grep -Fq "resolved metadata outside the ARM64 sysroot" "${TMP}/build-err"

if run_build open open-missing-kms target >"${TMP}/build-out" 2>"${TMP}/build-err"; then
  echo "FAIL: open build accepted an artifact without KMSDRM"
  exit 1
fi
grep -Fq 'FATAL: open SDL artifact is missing compiled KMSDRM evidence: kmsdrm' "${TMP}/build-err"

if run_build invalid ddk target >"${TMP}/build-out" 2>"${TMP}/build-err"; then
  echo "FAIL: build accepted an invalid GPU model"
  exit 1
fi
grep -Fq "FATAL: PF_GPU_MODEL must be 'ddk' or 'open'" "${TMP}/build-err"

echo "PASS: open/closed SDL build and artifact contracts fail closed (open: KMSDRM without sunxifb; ddk: sunxifb without KMSDRM)"
