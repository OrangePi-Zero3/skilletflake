# userspace/mesa: panvk for Windows

This directory builds Mesa's panvk Vulkan driver as `vulkan_panfrost.dll` and its ICD manifest
for Windows. The main target is aarch64 (RK3588 / Mali-G610, CSF v10). An x86_64 build is also
produced for wine tests.

- `MESA_COMMIT`: the pinned upstream Mesa commit.
- `patches/`: the series applied on top of the pin, with `git am`, in order.
  - Regenerate it from the `panwin` branch of `references/mesa-src` with:

    ```sh
    git format-patch --zero-commit --no-signature -o patches <pin>..panwin
    ```

  - **Every patch is AI-generated** (`Generated-by: LLM`). Mesa's `AGENTS.md` and the Panfrost
    AI policy (`docs/drivers/panfrost.rst`) ask that these not be submitted upstream as-is. A
    human has to rewrite them first.
- `cross/<arch>-w64-mingw32.ini`: the meson cross files for llvm-mingw. `build.sh` passes a
  generated paths file before them.
- `build.sh [aarch64|x86_64|all|checkout|host-tools]`: an idempotent build. It does the checkout
  and applies the series, builds the host precomp tools, finds libdrm-win (or falls back to a stub
  library), then runs meson + ninja + install. It finishes with import and export checks.
- `stub-libdrm/`: a no-device `libdrm.dll` stand-in. It is only used when libdrm-win is not
  installed.
- `tests/icd_smoke.c` and `tests/run-wine-smoke.sh`: a loader-less ICD smoke test, run under wine
  against the x86_64 build.

See `docs/status/mesa.md` for what was verified and what was not.
