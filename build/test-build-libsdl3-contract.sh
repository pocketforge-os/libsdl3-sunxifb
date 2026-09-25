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

make_stub readelf-ok \
  "printf '%s\\n' ' 0x1 (NEEDED) Shared library: [libEGL.so.1]' ' 0x1 (NEEDED) Shared library: [libGLESv2.so.2]'"
make_stub nm-ok "echo '00000000 T SDL_DYNAPI_entry'"
make_stub strings-ddk "printf '%s\\n' sunxifb dummy"
make_stub strings-open "printf '%s\\n' sunxifb kmsdrm dummy libdrm.so.2 libgbm.so.1"
make_stub strings-open-no-kms "printf '%s\\n' sunxifb dummy libdrm.so.2 libgbm.so.1"
make_stub strings-open-no-gbm "printf '%s\\n' sunxifb kmsdrm dummy libdrm.so.2"
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

if run_verify ddk "${TMP}/fail" "${TMP}/nm-ok" "${TMP}/strings-ddk" "${TMP}/symver-ok" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: verification accepted a failing readelf"
  exit 1
fi
grep -Fq 'FATAL: readelf inspection failed' "${TMP}/err"

if run_verify ddk "${TMP}/readelf-ok" "${TMP}/nm-ok" "${TMP}/fail" "${TMP}/symver-ok" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: verification accepted a failing strings tool"
  exit 1
fi
grep -Fq 'FATAL: strings inspection failed' "${TMP}/err"

if run_verify ddk "${TMP}/readelf-ok" "${TMP}/fail" "${TMP}/strings-ddk" "${TMP}/symver-ok" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: verification accepted a failing nm"
  exit 1
fi
grep -Fq 'FATAL: nm inspection failed' "${TMP}/err"

if run_verify ddk "${TMP}/readelf-ok" "${TMP}/nm-ok" "${TMP}/strings-ddk" "${TMP}/fail" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: verification accepted a failing symbol-version check"
  exit 1
fi
grep -Fq 'FATAL: glibc symbol-version inspection failed' "${TMP}/err"

make_stub readelf-rpath \
  "printf '%s\\n' ' 0x1 (NEEDED) Shared library: [libEGL.so.1]' ' 0x1 (NEEDED) Shared library: [libGLESv2.so.2]' ' 0x0 (RUNPATH) Library runpath: [/tmp]'"
if run_verify ddk "${TMP}/readelf-rpath" "${TMP}/nm-ok" "${TMP}/strings-ddk" "${TMP}/symver-ok" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: verification accepted RUNPATH"
  exit 1
fi
grep -Fq 'FATAL: shipped SDL artifact contains RPATH or RUNPATH' "${TMP}/err"

make_stub readelf-no-egl "echo ' 0x1 (NEEDED) Shared library: [libGLESv2.so.2]'"
if run_verify ddk "${TMP}/readelf-no-egl" "${TMP}/nm-ok" "${TMP}/strings-ddk" "${TMP}/symver-ok" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: verification accepted an artifact without libEGL"
  exit 1
fi
grep -Fq 'FATAL: SDL artifact is missing required dependency libEGL.so.1' "${TMP}/err"

# Build a real ELF negative control whose dynamic table directly links every
# graphics dependency while its exported symbol and strings otherwise resemble
# an acceptable open artifact. This must fail specifically on libdrm/libgbm
# DT_NEEDED rather than on a mocked readelf response.
HOST_CC=${CC:-cc}
for tool in "${HOST_CC}" readelf nm strings; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "FAIL: real direct-link ELF control requires ${tool}"
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
printf '%s\n' \
  'void SDL_DYNAPI_entry(void) {}' \
  'const char backend_sunxifb[] = "sunxifb";' \
  'const char backend_kmsdrm[] = "kmsdrm";' \
  'const char dynamic_libdrm[] = "libdrm.so.2";' \
  'const char dynamic_libgbm[] = "libgbm.so.1";' \
  >"${TMP}/direct-kms.c"
"${HOST_CC}" -shared -fPIC -Wl,--no-as-needed -L"${TMP}" \
  -o "${TMP}/direct-kms.so" "${TMP}/direct-kms.c" \
  -lEGL -lGLESv2 -ldrm -lgbm
if PF_GPU_MODEL=open READELF=readelf NM=nm STRINGS=strings \
    SYMVER_CHECK=${TMP}/symver-ok "${VERIFY}" "${TMP}/direct-kms.so" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: open verification accepted directly linked KMSDRM dependencies"
  exit 1
fi
grep -Fq 'FATAL: open SDL artifact links libdrm.so directly instead of using dynamic KMSDRM loading' "${TMP}/err"

run_verify ddk "${TMP}/readelf-ok" "${TMP}/nm-ok" "${TMP}/strings-ddk" "${TMP}/symver-ok" \
  >"${TMP}/out" 2>"${TMP}/err"
grep -Fq 'sunxifb' "${TMP}/out"

if run_verify ddk "${TMP}/readelf-ok" "${TMP}/nm-ok" "${TMP}/strings-ddk-with-kms" "${TMP}/symver-ok" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: closed verification accepted KMSDRM"
  exit 1
fi
grep -Fq 'FATAL: closed SDL artifact unexpectedly contains the KMSDRM backend' "${TMP}/err"

if run_verify ddk "${TMP}/readelf-ok" "${TMP}/nm-ok" "${TMP}/strings-no-sunxifb" "${TMP}/symver-ok" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: closed verification accepted a missing sunxifb backend"
  exit 1
fi
grep -Fq 'FATAL: closed SDL artifact is missing the sunxifb backend' "${TMP}/err"

run_verify open "${TMP}/readelf-ok" "${TMP}/nm-ok" "${TMP}/strings-open" "${TMP}/symver-ok" \
  >"${TMP}/out" 2>"${TMP}/err"
grep -Fq 'kmsdrm' "${TMP}/out"
grep -Fq 'sunxifb' "${TMP}/out"

if run_verify open "${TMP}/readelf-ok" "${TMP}/nm-ok" "${TMP}/strings-open-no-kms" "${TMP}/symver-ok" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: open verification accepted a missing KMSDRM backend"
  exit 1
fi
grep -Fq 'FATAL: open SDL artifact is missing compiled KMSDRM evidence: kmsdrm' "${TMP}/err"

if run_verify open "${TMP}/readelf-ok" "${TMP}/nm-ok" "${TMP}/strings-open-no-gbm" "${TMP}/symver-ok" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: open verification accepted non-dynamic KMSDRM evidence"
  exit 1
fi
grep -Fq 'FATAL: open SDL artifact is missing compiled KMSDRM evidence: libgbm.so.1' "${TMP}/err"

if run_verify bogus "${TMP}/readelf-ok" "${TMP}/nm-ok" "${TMP}/strings-ddk" "${TMP}/symver-ok" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: verification accepted an invalid GPU model"
  exit 1
fi
grep -Fq "FATAL: PF_GPU_MODEL must be 'ddk' or 'open'" "${TMP}/err"

# Exercise the canonical build entry point with controlled tools. This proves
# model selection reaches both CMake and the real artifact verifier while also
# supplying negative controls for missing/host target metadata.
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
make_stub strings-build \
  'case "${ARTIFACT_KIND:-${PF_GPU_MODEL:-ddk}}" in' \
  '  open) printf "%s\\n" sunxifb kmsdrm dummy libdrm.so.2 libgbm.so.1 ;;' \
  '  open-missing-kms) printf "%s\\n" sunxifb dummy libdrm.so.2 libgbm.so.1 ;;' \
  '  ddk) printf "%s\\n" sunxifb dummy ;;' \
  '  *) exit 2 ;;' \
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
    READELF=${TMP}/readelf-ok \
    NM=${TMP}/nm-ok \
    STRINGS=${TMP}/strings-build \
    SYMVER_CHECK=${TMP}/symver-ok \
    OUT=${out} \
    SRC=${TMP}/source \
    BLOBS=${blobs_root} \
    PF_GPU_MODEL=${model} \
    SUNXIFB_MESA_ROOT=${mesa_root} \
    "${SCRIPT}"
}

run_build open open target >"${TMP}/build-out" 2>"${TMP}/build-err"
grep -Fq -- '-DSDL_KMSDRM=ON' "${TMP}/cmake-open-open-target.log"
grep -Fq -- '-DSDL_KMSDRM_SHARED=ON' "${TMP}/cmake-open-open-target.log"
grep -Fq 'kmsdrm' "${TMP}/build-out"
grep -Fq 'sunxifb' "${TMP}/build-out"

run_build ddk ddk target >"${TMP}/build-out" 2>"${TMP}/build-err"
grep -Fq -- '-DSDL_KMSDRM=OFF' "${TMP}/cmake-ddk-ddk-target.log"
grep -Fq 'sunxifb' "${TMP}/build-out"

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

echo "PASS: open/closed SDL build and artifact contracts fail closed"
