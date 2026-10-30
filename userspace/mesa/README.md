# userspace/mesa: panvk for Windows

This directory builds Mesa's panvk Vulkan driver as `vulkan_panfrost.dll` and its ICD manifest
for Windows. The main target is aarch64: panvk v7 for the Mali-G31 (Orange Pi Zero3, job
manager) and panvk v10 for the Mali-G610 (RK3588, CSF). An x86_64 build is also produced for
wine tests.

The Mesa used here is a hard fork, locked at one upstream commit and built from that commit's
source tarball plus one diff. Read `AI-POLICY.md` before changing it.

- `MESA_COMMIT`: the upstream commit the fork is locked to.
- `MESA_TARBALL_SHA256`: the sha256 of
  `https://gitlab.freedesktop.org/mesa/mesa/-/archive/<commit>/mesa-<commit>.tar.gz`.
- `patches/panwin-mesa.diff`: the fork, as one diff against that tarball. Its `#` header lists
  the commits it was made from. Every change is LLM-written (`Generated-by: LLM`) and never goes
  upstream.
- `build.sh [aarch64|x86_64|all|checkout|host-tools|diff]`: an idempotent build.
  - It downloads and checks the tarball once (`references/mesa-tarball`), extracts it and
    applies the diff (`references/mesa-panwin`).
  - It builds the host precomp tools, finds libdrm-win (or falls back to a stub library), then
    runs meson + ninja + install. It finishes with import and export checks.
  - When the diff changes, the old one is reversed and the new one applied in place, so only the
    touched files rebuild.
- `cross/<arch>-w64-mingw32.ini`: the meson cross files for llvm-mingw. `build.sh` passes a
  generated paths file before them.
- `stub-libdrm/`: a no-device `libdrm.dll` stand-in. It is only used when libdrm-win is not
  installed.
- `tests/icd_smoke.c` and `tests/run-wine-smoke.sh`: a loader-less ICD smoke test, run under wine
  against the x86_64 build.

## Changing the fork

The fork is developed as branch `panwin` of a git checkout of Mesa (`references/mesa-src`, based
on `MESA_COMMIT`). Commit there with the `Generated-by: LLM` trailer, then export:

```sh
userspace/mesa/build.sh diff
```

To build the git checkout directly while iterating, skip the tarball step:

```sh
MESA_SRC=../references/mesa-src PANWIN_MESA_NO_RESET=1 userspace/mesa/build.sh x86_64
```

Moving to a newer Mesa means rebasing the branch, then updating `MESA_COMMIT`,
`MESA_TARBALL_SHA256` and the diff together.

See `docs/status/mesa.md` for what was verified and what was not.
