#!/bin/sh
# Fail-closed inspection of a built PocketForge SDL shared library.
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: PF_GPU_MODEL=<ddk|open> $0 <libSDL3-pocketforge.so>" >&2
  exit 2
fi

SO=$1
PF_GPU_MODEL=${PF_GPU_MODEL:-ddk}
READELF=${READELF:-aarch64-none-linux-gnu-readelf}
NM=${NM:-aarch64-none-linux-gnu-nm}
STRINGS=${STRINGS:-aarch64-none-linux-gnu-strings}
SYMVER_CHECK=${SYMVER_CHECK:-/usr/local/bin/check-glibc-symver}

if ! dynamic=$("${READELF}" -d "${SO}"); then
  echo "FATAL: readelf inspection failed for SDL artifact" >&2
  exit 1
fi
if printf '%s\n' "${dynamic}" | grep -E '\((RPATH|RUNPATH)\)' >/dev/null; then
  echo "FATAL: shipped SDL artifact contains RPATH or RUNPATH" >&2
  exit 1
fi
for needed in libEGL.so.1 libGLESv2.so.2; do
  if ! printf '%s\n' "${dynamic}" | grep -F '(NEEDED)' | grep -F "[${needed}]" >/dev/null; then
    echo "FATAL: SDL artifact is missing required dependency ${needed}" >&2
    exit 1
  fi
done

if ! symbols=$("${NM}" -D "${SO}"); then
  echo "FATAL: nm inspection failed for SDL artifact" >&2
  exit 1
fi
if ! printf '%s\n' "${symbols}" | grep 'DYNAPI_entry' >/dev/null; then
  echo "FATAL: SDL_DYNAPI_entry not exported" >&2
  exit 1
fi

if ! artifact_strings=$("${STRINGS}" "${SO}"); then
  echo "FATAL: strings inspection failed for SDL artifact" >&2
  exit 1
fi
echo "SDL video backends:"
printf '%s\n' "${artifact_strings}" \
  | grep -E '^(x11|wayland|kmsdrm|sunxifb|dummy|offscreen|vivante|rpi)$' \
  | sort -u || true

has_artifact_string() {
  # Read the complete stream rather than using grep -q: the latter exits at
  # the first match and can make dash's printf report a broken-pipe I/O error
  # for a large real library's strings output.
  printf '%s\n' "${artifact_strings}" | grep -Fx "$1" >/dev/null
}

case "${PF_GPU_MODEL}" in
  open)
    for required in sunxifb kmsdrm libdrm.so.2 libgbm.so.1; do
      if ! has_artifact_string "${required}"; then
        echo "FATAL: open SDL artifact is missing compiled KMSDRM evidence: ${required}" >&2
        exit 1
      fi
    done
    ;;
  ddk)
    if ! has_artifact_string sunxifb; then
      echo "FATAL: closed SDL artifact is missing the sunxifb backend" >&2
      exit 1
    fi
    if has_artifact_string kmsdrm; then
      echo "FATAL: closed SDL artifact unexpectedly contains the KMSDRM backend" >&2
      exit 1
    fi
    ;;
  *)
    echo "FATAL: PF_GPU_MODEL must be 'ddk' or 'open' for artifact verification, got '${PF_GPU_MODEL}'" >&2
    exit 1
    ;;
esac

if ! "${SYMVER_CHECK}" "${SO}"; then
  echo "FATAL: glibc symbol-version inspection failed for SDL artifact" >&2
  exit 1
fi
