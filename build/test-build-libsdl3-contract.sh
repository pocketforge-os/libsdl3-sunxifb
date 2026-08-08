#!/bin/sh
# Cheap source contract for invariants that normally run in the pinned
# cross-build container.
set -eu

SCRIPT=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)/build-libsdl3.sh

require_literal() {
  if ! grep -Fq -- "$1" "${SCRIPT}"; then
    echo "FAIL: canonical build recipe is missing: $1"
    exit 1
  fi
}

require_literal '-DSDL_RPATH=OFF'
require_literal '-DCMAKE_SKIP_RPATH=ON'
require_literal "grep -Eq '\\((RPATH|RUNPATH)\\)'"
require_literal 'FATAL: shipped SDL artifact contains RPATH or RUNPATH'

echo "PASS: canonical build recipe disables and rejects ELF runtime paths"
