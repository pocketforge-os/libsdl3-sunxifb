#!/bin/sh
# Cheap source contract for invariants that normally run in the pinned
# cross-build container.
set -eu

SCRIPT=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)/build-libsdl3.sh
VERIFY=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)/verify-libsdl3-artifact.sh

require_literal() {
  if ! grep -Fq -- "$1" "${SCRIPT}"; then
    echo "FAIL: canonical build recipe is missing: $1"
    exit 1
  fi
}

require_literal '-DSDL_RPATH=OFF'
require_literal '-DCMAKE_SKIP_RPATH=ON'
require_literal 'verify-libsdl3-artifact.sh'

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

make_stub readelf-ok "echo 'Dynamic section has no runtime path'"
make_stub nm-ok "echo '00000000 T SDL_DYNAPI_entry'"
make_stub strings-ok "printf '%s\\n' sunxifb dummy"
make_stub symver-ok 'exit 0'
make_stub fail 'exit 23'

run_verify() {
  READELF=$1 NM=$2 STRINGS=$3 SYMVER_CHECK=$4 "${VERIFY}" "${SO}"
}

if run_verify "${TMP}/fail" "${TMP}/nm-ok" "${TMP}/strings-ok" "${TMP}/symver-ok" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: verification accepted a failing readelf"
  exit 1
fi
grep -Fq 'FATAL: readelf inspection failed' "${TMP}/err"

if run_verify "${TMP}/readelf-ok" "${TMP}/nm-ok" "${TMP}/fail" "${TMP}/symver-ok" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: verification accepted a failing strings tool"
  exit 1
fi
grep -Fq 'FATAL: strings inspection failed' "${TMP}/err"

if run_verify "${TMP}/readelf-ok" "${TMP}/fail" "${TMP}/strings-ok" "${TMP}/symver-ok" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: verification accepted a failing nm"
  exit 1
fi
grep -Fq 'FATAL: nm inspection failed' "${TMP}/err"

if run_verify "${TMP}/readelf-ok" "${TMP}/nm-ok" "${TMP}/strings-ok" "${TMP}/fail" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: verification accepted a failing symbol-version check"
  exit 1
fi
grep -Fq 'FATAL: glibc symbol-version inspection failed' "${TMP}/err"

make_stub readelf-rpath "echo ' 0x0 (RUNPATH) Library runpath: [/tmp]'"
if run_verify "${TMP}/readelf-rpath" "${TMP}/nm-ok" "${TMP}/strings-ok" "${TMP}/symver-ok" \
    >"${TMP}/out" 2>"${TMP}/err"; then
  echo "FAIL: verification accepted RUNPATH"
  exit 1
fi
grep -Fq 'FATAL: shipped SDL artifact contains RPATH or RUNPATH' "${TMP}/err"

run_verify "${TMP}/readelf-ok" "${TMP}/nm-ok" "${TMP}/strings-ok" "${TMP}/symver-ok" \
  >"${TMP}/out" 2>"${TMP}/err"
grep -Fq 'sunxifb' "${TMP}/out"
grep -Fq 'dummy' "${TMP}/out"

echo "PASS: artifact inspection fails closed and rejects ELF runtime paths"
