# libsdl3-sunxifb

SDL3 mainline + forward-ported `sunxifb` video backend (from the vendor `SDL2-2.26.1.GE8300` `mali-fbdev` patch). Produces **`libSDL3-pocketforge.so.0`** — the single SDL3 shared library that:

- Steam Link loads at runtime via `SDL3_DYNAMIC_API` (the official Valve dynapi override mechanism)
- The PocketForge launcher links against directly
- All first-party PocketForge apps link against

Built with ARM A-Profile Toolchain 10.3-2021.07 (gcc 10.3.1 / glibc 2.33) in a reproducible cross-build container. Tags on this repo imply ABI/dynapi-version commitments.

Populated in **Phase 0** (bead `tsp-8si.8`+). See the [pocketforge-os](https://github.com/pocketforge-os) org for the full repo set.
