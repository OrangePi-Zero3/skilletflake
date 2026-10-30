# Mesa AI policy: read before using these patches

Mesa's `AGENTS.md` tells AI agents not to modify `src/panfrost` and `src/gallium/drivers/panfrost`.
It refers users to the Panfrost AI policy instead (`docs/drivers/panfrost.rst`, section
"AI policy"). That policy asks developers not to use AI to generate patches and to write the code
they submit themselves.

Status of the series in `patches/`:

- Every patch was LLM-generated and carries `Generated-by: LLM`. None may be submitted upstream as is.
- **6 patches touch `src/panfrost` against that policy:** 0008, 0009, 0010, 0011, 0012 and 0014. They
  are a local, working prototype that proves panvk can build and enumerate on Windows. They are
  waiting on the project owner's decision: keep them as a private downstream prototype, or have a
  human rewrite them.
- 0001–0007 and 0013 touch only `src/util`, `src/vulkan` and `meson.build`. Those areas are covered by
  the general Mesa rule: label the commits, never open MRs or post comments upstream.

Going forward, no AI-authored changes are made under `src/panfrost`. Windows-specific behaviour
should go into layers outside it: libdrm-win, `src/util` (os_mmap, build-id), compat headers
force-included from the cross file, and the kernel driver accepting what unmodified panvk sends.
That way the remaining `src/panfrost` delta stays as small as possible, for a human to write.
