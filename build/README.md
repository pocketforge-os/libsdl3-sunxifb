# PocketForge cross-build container

Reproducible cross-build for `libSDL3-pocketforge.so.0` and (later) the full
PocketForge SD-image pipeline. Targets the TrimUI Smart Pro: 64-bit ARM,
Cortex-A53 / A133P SoC, **glibc 2.33** rootfs.

The toolchain pin is non-negotiable. See [why](#why-the-toolchain-pin) below.

## Build the image

```sh
docker build \
    -t pocketforge/cross-build:10.3-2021.07-bookworm \
    build/
```

The two-stage build downloads the ARM A-Profile Toolchain `10.3-2021.07`
(~250 MB), verifies SHA-256 + MD5, unpacks under `/opt/arm-10.3-2021.07/`,
strips docs/manpages, then builds the final stage on `debian:bookworm-slim`
with build-essentials and image-composition tools (`dtc abootimg genimage
dosfstools mtools sunxi-tools debootstrap qemu-user-static`...). Final image
size ~880 MB.

A build-time hello-world + symver gate runs at the end of stage 2; if it
references any GLIBC_2.34+ symbol the image build aborts.

## Use the image

The container expects a documented bind-mount layout:

| Mount | Mode | Purpose |
| --- | --- | --- |
| `/work/src` | read-only | source tree (e.g. `libsdl3-sunxifb` checkout) |
| `/work/blobs` | read-only | PowerVR DDK from the `blobs` repo |
| `/work/out` | read-write | artifact output |

Run with `--user $(id -u):$(id -g)` so artifacts dropped into `/work/out`
are owned by the calling host user, not by root.

Example shape (for an out-of-tree CMake build of the eventual SDL3 port):

```sh
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v "$(pwd):/work/src:ro" \
    -v "$(realpath ../blobs):/work/blobs:ro" \
    -v "$(pwd)/out:/work/out" \
    pocketforge/cross-build:10.3-2021.07-bookworm \
    bash -c '
        cmake -G Ninja \
              -DCMAKE_TOOLCHAIN_FILE=/opt/cmake/toolchain-arm-10.3-2021.07.cmake \
              -DCMAKE_BUILD_TYPE=Release \
              -B /work/out \
              -S /work/src
        cmake --build /work/out
    '
```

## CMake toolchain file

Shipped at `/opt/cmake/toolchain-arm-10.3-2021.07.cmake`. Symlink convenience
path at `/opt/cmake/toolchain.cmake`. Pass it via `-DCMAKE_TOOLCHAIN_FILE=`.

Sysroot is `/opt/arm-10.3-2021.07/aarch64-none-linux-gnu/libc`. It has glibc
2.33 + drm headers + pthread.h, but **no EGL/GLES2** — those live in the
PowerVR DDK and bind-mount in via `/work/blobs`. Downstream CMake should
append `/work/blobs/path/to/ddk-includes` to `CMAKE_FIND_ROOT_PATH`; the
toolchain file is structured so that this is purely additive.

The triplet is `aarch64-none-linux-gnu` (note the `-none-`, not the Linux
convention `-linux-`). SDL3's CMake doesn't care about the triplet; do NOT
manufacture short-name symlinks — the toolchain has zero by design.

## Verify a built artifact

```sh
docker run --rm \
    -v "$(pwd)/out:/work/out:ro" \
    pocketforge/cross-build:10.3-2021.07-bookworm \
    check-glibc-symver /work/out/some-binary.elf
```

Or run `build/check-glibc-symver.sh` directly on the host (only requires
`readelf` from binutils). The script exits non-zero if any binary references
a GLIBC_2.34+ symbol — suitable as a CI gate before publishing artifacts.

## Deploy to TSP

Build artifact emerges into `/work/out` on the host. Push to the device with
the deploy harness (bead **tsp-8si.4**, sibling). Sketch:

```sh
rsync -av --partial --progress out/libSDL3-pocketforge.so.0 \
      "${TSP_HOST:-root@192.168.86.169}:/usr/lib/"
ssh "${TSP_HOST:-root@192.168.86.169}" \
    "ldconfig && readelf -V /usr/lib/libSDL3-pocketforge.so.0 | grep GLIBC"
```

(See the deploy harness's `scripts/README.md` for the canonical invocation
once that bead lands.)

## Why the toolchain pin

Stock Ubuntu 24.04 `gcc-aarch64-linux-gnu` is gcc-13 / glibc 2.34. TSP runs
glibc 2.33 and rejects 2.34-versioned binaries at startup with `version
'GLIBC_2.34' not found`. Workarounds investigated and **rejected**:

- `.symver`-wrap to redirect 2.34 symbols → fails because `__libc_start_main`
  is the typical 2.34 culprit and isn't user-facing
- musl-static → fails because vendor `libsrv_um.so` requires glibc-only
  symbols (`backtrace`, `backtrace_symbols`, `__strdup`)
- bundled glibc 2.39 → fails because vendor libs transitively link
  `librt.so.1` from glibc 2.33 and reject 2.39's `GLIBC_PRIVATE` symbols

**The pinned canonical toolchain is ARM A-Profile `10.3-2021.07`**:

- URL: `https://armkeil.blob.core.windows.net/developer/Files/downloads/gnu-a/10.3-2021.07/binrel/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu.tar.xz`
- SHA-256: `1e33d53dea59c8de823bbdfe0798280bdcd138636c7060da9d77a97ded095a84`
- MD5 (Arm-published): `07bbe2b5277b75ba36a924e9136366a4`
- gcc 10.3.1, glibc 2.33

Both checksums are pinned in the Dockerfile and verified at fetch time. If
Arm rotates the URL, **do not silently substitute** — surface to the user;
archived-URL fallback is documented in
`build-integration-reference.md` §3.6.

For full forensics see:
- `build-integration-reference.md` §3.6 — toolchain pin rationale
- `hardware-firmware-probes.md` §11.5 — failed-workaround inventory
- `pocketforge-plan.md` §15 row 15 — Risk #15 (build-host cross-toolchain pin)

## When this Dockerfile moves

Per `pocketforge-plan.md` §14 and `AGENTS.md`, this container migrates to the
`image` repo when the full SD-image pipeline is built (Phase 1). Until then
it lives here in `libsdl3-sunxifb` because Phase 0 work (the SDL3 forward-
port) is the only consumer, and its CI lane wants the Dockerfile next to the
source it builds.

## Fallback if the container becomes burdensome

Per `AGENTS.md`: if the container pipeline blocks Phase 0 progress for more
than ~half a day (slow iteration, opaque failures), **stop and ask the user**
before pivoting. The fallback is bare cross-compile on the host with the
toolchain unpacked at `/opt/arm-10.3-2021.07/` (same toolchain, same sysroot
— just no container layer). The toolchain file at
`/opt/cmake/toolchain-arm-10.3-2021.07.cmake` works identically on host and
container because all paths are absolute.
