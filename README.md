# panfrost-win

An open-source (MIT) GPU driver for Arm Mali GPUs on Windows on ARM64, starting with the
Rockchip RK3588's Mali-G610 (Valhall, CSF). It pairs a small Windows kernel driver (`panwin.sys`)
that speaks the Linux panthor uAPI with Mesa's panvk Vulkan driver, cross-built for Windows.

See [docs/PLAN.md](docs/PLAN.md) for the architecture, decisions and milestones, and
[docs/research/](docs/research/) for the background research.

Status: M0 (foundations, no hardware yet).
