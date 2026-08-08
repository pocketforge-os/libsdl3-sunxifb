#!/bin/sh
# Fail-closed inspection of a built PocketForge SDL shared library.
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <libSDL3-pocketforge.so>" >&2
  exit 2
fi

SO=$1
READELF=${READELF:-aarch64-none-linux-gnu-readelf}
NM=${NM:-aarch64-none-linux-gnu-nm}
STRINGS=${STRINGS:-aarch64-none-linux-gnu-strings}
SYMVER_CHECK=${SYMVER_CHECK:-/usr/local/bin/check-glibc-symver}

if ! dynamic=$("${READELF}" -d "${SO}"); then
  echo "FATAL: readelf inspection failed for SDL artifact" >&2
  exit 1
fi
if printf '%s\n' "${dynamic}" | grep -Eq '\((RPATH|RUNPATH)\)'; then
  echo "FATAL: shipped SDL artifact contains RPATH or RUNPATH" >&2
  exit 1
fi

if ! symbols=$("${NM}" -D "${SO}"); then
  echo "FATAL: nm inspection failed for SDL artifact" >&2
  exit 1
fi
if ! printf '%s\n' "${symbols}" | grep -q 'DYNAPI_entry'; then
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

if ! "${SYMVER_CHECK}" "${SO}"; then
  echo "FATAL: glibc symbol-version inspection failed for SDL artifact" >&2
  exit 1
fi
