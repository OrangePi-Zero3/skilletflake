# Prior art and alternative strategies: a Mali GPU driver on Windows on Arm

Author: prior-art research subagent. Date: 2026-10-30.

Legend:
- **[V]** verified. I read the code, file or page myself (path or URL given).
- **[S]** taken from a secondary source or a search-result snippet. The primary source was not read.
- **[I]** my own inference or judgement.

Reference clones made for this topic (all shallow):

| Path | Upstream | Commit |
|---|---|---|
| `references/priorart-osy-viogpu-kmd` | github.com/osy/kvm-guest-drivers-windows, branch `neptune` | `ed82100` (2026-10-28) |
| `references/priorart-ms-graphics-driver-samples` | github.com/microsoft/graphics-driver-samples | `de4a216` (2023-06-13) |
| `references/priorart-dxvk` (sparse: `src/dxvk`, `src/d3d11`, `src/d3d9`) | github.com/doitsujin/dxvk | `78cdf8f` (2026-10-29) |
| `references/priorart-vkd3d-proton` (sparse: `libs/vkd3d`) | github.com/HansKristian-Work/vkd3d-proton | `4522cc1` (2026-10-27) |

Some files I only fetched into the scratchpad and did not clone: Mesa `main` files from gitlab.freedesktop.org, and `src/virtio/vulkan/vn_renderer_virtgpu_win32.c` from github.com/osy/virtio-win-mesa (branch `neptune`).

---

## 0. TL;DR

1. **No open-source or public Mali driver for Windows exists.** [V] for RK3588: the worproject Rockchip-Windows-Drivers table lists GPU as "Not working, software-rendered", and a maintainer (idigdoug) wrote in Jan 2025 that "Nobody is currently working on GPU driver" (https://github.com/worproject/Rockchip-Windows-Drivers/issues/54).
   - Arm said in 2022 it had "no immediate plans" for a Windows Immortalis driver ([V] https://www.pcworld.com/article/796784/).
   - Arm job postings from Oct 2024 to Feb 2025 in Trondheim do describe building "the DirectX 12 driver for the latest Mali GPU" and "Direct3D drivers for Windows on Arm". [S] The postings now return 404.
   - **So an Arm-official D3D11/12 Mali driver is probably in development.** It most likely targets new Mali generations and OEM SoCs (MediaTek and others), not Valhall-gen3 G610 on hobbyist RK3588 boards. [I] Strategically this matters: an official driver for G610 is unlikely, but one for future SoCs is plausible.
2. **The closest prior art is the 2023–2026 VirtIO-GPU 3D work for Windows guests, not anything Mali-related.** It is a family of Mesa-on-Windows WDDM stacks:
   - max8rr8 viogpu3d
   - anonymix007's Venus KMD and his Rust WDDM 2.0 KMD
   - arehnman's "Yttrium"
   - osy/UTM's "Neptune/Triton"

   Together they prove that a **third party can ship a working WDDM 2.x KMD plus Mesa UMDs (Vulkan ICD, D3D9/10/11 UMD, OpenGL via Zink) on Windows, including Windows 11 ARM64 with an ARM64X UMD**, and run real games (3DMark Fire Strike, Crash Bandicoot, Unigine).

   The one piece directly reusable as a template is **Venus's Windows renderer** (`vn_renderer_virtgpu_win32.c`, 2276 lines). It shows a Mesa Vulkan driver whose kernel interface is reimplemented over `D3DKMT*` thunks (CreateAllocation, Lock, Render, Escape, ...) against a custom WDDM KMD. It is the exact analogue of "panvk + a Windows kmod backend".
3. **Microsoft's own `graphics-driver-samples` (MIT) contains a WDDM driver for the Raspberry Pi 2 VC4 GPU.** This is the Render-Only Sample (ROS): `roskmd` is about 11.3k lines, `rosumd` (D3D11 UMD) about 8.2k, and `roscompiler` about 5.1k. The repo also has a Compute-Only Sample whose `cosumd12` is a D3D12 UMD (about 9.7k lines). It is an arm32/x64 reference for a UMA SoC GPU under WDDM, and it is the origin of the "roskmd.sys" that worproject ships, which "loads but does little".
4. **API coverage with only a Vulkan driver (panvk on Windows), as Mesa/DXVK stand today:**
   - **Vulkan 1.4:** panvk is conformant on G610 on Linux.
   - **OpenGL via Zink:** but panvk lacks geometry shaders, tessellation, clip/cull distance, `fillModeNonSolid` and multiViewport. **Zink is therefore capped around GL 3.1 / GLES 3.1** until panvk gains GS/tess (GS work via `libpoly` is in flight, Mesa MR !44118, Oct 2026, behind a driconf). [I] for the exact cap.
   - **DXVK (D3D9/10/11): does not run at all today.** DXVK hard-requires `geometryShader`, `fillModeNonSolid`, `multiViewport`, `shaderClipDistance`, `shaderCullDistance` and others ([V] `src/dxvk/dxvk_device_info.cpp:847-1066`).
   - **vkd3d-proton (D3D12):** assumes FL 11_0 as the minimum and needs bindless-scale descriptors. [I] Realistically it is out of reach for a long time.
   - **No system-level D3D at all without a WDDM UMD.** Only app-local DXVK DLLs would work.
   - **x64-emulated apps (most games) need an ARM64X/ARM64EC ICD and UMD.** [V] DXVK issue #5125. [V] osy's BUILDING.md.
5. **API coverage with a D3D12 UMD + WDDM KMD:**
   - D3D12
   - D3D11/10 via the OS "11on12" mode (`D3D11_UMD_MODE_SYSTEM11ON12`, [V] ntdoc)
   - D3D9 via D3D9On12 (an INF trick, [V] osy BUILDING.md)
   - GL via GLOn12 (Mesa d3d12, GL 4.6 in Mesa features.txt [V], 3.3 per the Store pack text [S])
   - CL via CLOn12
   - Vulkan via Dozen (only Vulkan 1.0 complete in features.txt [V])

   This is the "Microsoft-blessed" coverage path, which Intel Arc (DX9 via D3D9On12) and Qualcomm also lean on. **But D3D12 requires FL 11_0 (GS+tess, bindless tiers, GPU VA / WDDM 2.x).** Writing a D3D12 UMD from scratch is a multi-person-year effort, and Arm's own team is doing it. [I]
6. **Recommendation [I]:** keep the leading hypothesis (RK3588/G610, panthor-like KMD, panvk with a new kmod backend). Make the KMD a **WDDM 2.x render+display KMD from day one, modelled on viogpu3d/ROS**, not a bare KMDF device. Reasons:
   - Vulkan can be discovered without WDDM, but OpenGL ICD selection, every D3D path, DWM acceleration and presentation all need WDDM.
   - The prior art shows the WDDM skeleton is roughly 15–25k lines and is doable.

   Sequence the work as:
   1. KMDF/WDDM bring-up with Mali FW boot and a CSF ring.
   2. Vulkan compute.
   3. Graphics with a CPU-copy present.
   4. WDDM present and DWM.
   5. Zink GL.
   6. D3D9–11 via a Gallium D3D frontend (Yttrium-style) or via DXVK once panvk has GS/tess.
   7. D3D12 last, or never.

---

## 1. Third-party / open-source GPU drivers for Windows

### 1.1 Windows on Raspberry Pi: VC4 (worproject + Microsoft ROS)

- **Status [V]** (https://github.com/worproject/RPi-Windows-Drivers):
  - "VC4 GPU (Graphics) | roskmd.sys | Not working | the driver loads, but it doesn't do much as it's unfinished".
  - Display goes through the in-box Basic Display Driver (MSBDD) on the firmware framebuffer.
  - The repo publishes binaries only.
- **Origin of roskmd [V]:** https://github.com/microsoft/graphics-driver-samples, cloned to `references/priorart-ms-graphics-driver-samples`.
  - README: "The first sample being developed is a Windows graphics driver for the Raspberry Pi 2 ... functioning ... but not a fully featured driver. Work on the driver was started in August 2015."
  - It "is capable of some basic D3D rendering including ... Dolphin".
  - License: MIT, copyright Microsoft. Last commit 2023-06-13 (mandatory-file bot). The repo is effectively dormant since about 2017. [I]
  - Sizes (`.cpp`/`.h`/`.c` line counts) [V]:

    | Component | Lines |
    |---|---|
    | `render-only-sample/roskmd` | 11,279 |
    | `rosumd` (D3D11 UMD) | 8,209 |
    | `roscompiler` (HLSL/DXBC to VC4) | 5,087 |
    | `compute-only-sample/coskmd` | 7,417 |
    | `cosumd12` (a D3D12 UMD for an MCDM compute-only device) | 9,676 |

  - Architecture [V]: WDDM KMD with a registry-selected `RenderOnly` mode (`roskmd/RosKmdGlobal.cpp:175-226`). It uses a carved-out "video memory" segment that the KMD pages with the CPU (`roskmd/RosKmdAdapter.cpp:229-275`), with VC4 control lists built in the KMD/UMD (`roskmd/RosKmdUtil.cpp`, `Vc4Hw.h`).
  - The display path is not via the VC4 HVS. ROS either runs render-only, or in full mode presents to a firmware framebuffer. [I] from the grep. There is no HVS/PixelValve code.
  - Value to us:
    - An MIT-licensed, Microsoft-written example of a WDDM driver for an **Arm SoC UMA GPU**, including a D3D11 UMD and a D3D12 UMD (`cosumd12` has `GpuVa` variants of command lists/queues, i.e. WDDM 2.x GPU VA).
    - A good skeleton to read for DDI sequencing. It is not up to date (WDDM 2.0-era) and is not production quality.
- **TheMindVirus/VC4WDDM2** (https://github.com/TheMindVirus/VC4WDDM2) [S]: a "skeleton DXGK driver on top of ACPI\VC4". It is a template only and renders nothing.
- **Where it stalled [I]:** VC4 is a GLES2-class GPU that cannot do D3D FL 9_3 properly. Microsoft lost interest when Windows IoT Core on Pi faded, and the community never had the bandwidth.
  - Lesson: the hardware must be able to meet the API floor (D3D FL 9_x / 11_0) or you build a toy.
  - **Mali-G610 is far more capable**: Vulkan 1.4 conformant, GLES 3.1 conformant, with GS/tess emulation possible as Arm's blob does.

### 1.2 VirtIO-GPU 3D for Windows guests: the most relevant prior art

Five projects form one evolving lineage, all Mesa + custom WDDM KMD:

| Project | Repos | Architecture | Achieved | Stalled / limits |
|---|---|---|---|---|
| **max8rr8 viogpu3d** (Jul 2023) | PR https://github.com/virtio-win/kvm-guest-drivers-windows/pull/943; Mesa MR !24223; virglrenderer MR !1185 | WDDM KMD `viogpu3d` + Mesa Gallium **virgl** driver + **d3d10umd** (Gallium D3D10 UMD) + **WGL** OpenGL ICD | wglgears, a D3D10 cube, DWM working; tested Win10 22H2 [V via PR page] | No preemption ("disabled system-wide as a workaround"), glitches in WinUI3, black Electron windows, BSODs (DRIVER_CORRUPTED_EXPOOL), 4K bugcheck. **PR never merged**; called "dead" by a contributor in Jul 2026. The upstream maintainer gave little help [V Neptune blog]. |
| **anonymix007 Venus KMD** (2025) | github.com/anonymix007/kvm-guest-drivers-windows-venus | viogpu3d fork + Mesa **Venus** Vulkan ICD on D3DKMT | Vulkan in Windows guests | Superseded by his Rust KMD |
| **anonymix007 Rust WDDM 2.0** (Aug 2026→) | https://github.com/anonymix007/virtio-drivers-windows-rs (KMD, Rust, AGPL-3), https://github.com/anonymix007/virtio-d3d11 (UMD, C, AGPL-3) | "VirtIO-GPU full (render + display) WDDM 2.0 kernel-mode driver" + own D3D11 UMD. **Built on Linux with clang-cl + EWDK via a VFS overlay** (tmp64/clang-cl-linux) [V README] | Gallery screenshot of the desktop | Active. The README stresses: "Low-level Windows development is rather complicated due to lack of documentation. Windows has so many ... undocumented quirks ... that writing code is not the bottleneck" [V]. LLM-generated contributions are prohibited. |
| **arehnman Yttrium** (Sep 2025→) | https://github.com/arehnman/yttrium-virtio-gpu (KMD, branches `yttrium-wddm2`, `akre`), https://github.com/arehnman/virtio-win-mesa branch `yttrium-experimental` | WDDM KMD + Mesa **d3d10umd extended with a D3D9 UMD** (`D3D9Adapter.cpp`, `D3D9NineShader.c` ...) on Gallium **Zink → Venus**; OpenGL via Zink; Vulkan via Venus; 32- and 64-bit UMDs [V tree + README] | "Passed the project's Direct3D Wine test suites", runs Unigine Superposition/Sanctuary, FurMark; target Win10 x64 on an Intel Arc host [V README] | Alpha; "may crash or hang a guest"; no Fast Startup. **Upstream Mesa removed d3d10umd in Mesa 26.3 (Oct 2026) because it is TGSI-only** [S Phoronix https://www.phoronix.com/news/Mesa-Drops-Direct3D-10-UMD], so this path now lives in forks. |
| **osy / UTM Neptune + Triton** (Mar–Oct 2026) | https://github.com/osy/kvm-guest-drivers-windows (`neptune`, KMD), https://github.com/osy/virtio-win-mesa (`neptune`, UMD), blogs https://blog.getutm.app/2026/introducing-neptune-direct3d-virtualization-for-qemu/ and https://blog.getutm.app/2026/introducing-triton-directx-11-driver-for-qemu/ | KMD `viogpu3d` (based on anonymix007's Venus KMD) + **Triton**, a D3D11 DDI UMD that turns DDI calls back into D3D11 API calls and forwards them (Neptune protocol) to a host D3D11 (DXVK/DXMT/D3DMetal). Also ships the Venus ICD. **ARM64X UMD** for x64-emulated apps; separate x86 WOW UMD [V `viogpu/viogpu3d/BUILDING.md`] | Windows 11 **ARM64** guest on Apple Silicon running x64 games (Crash Bandicoot, 3DMark Fire Strike) [V blog] | "Still very unstable" [V]. Shader-container (DXBC signature) synthesis is "the weakest and most error prone part" [V]. Heavily AI-assisted (byline "osy+claude-opus-4-8") [V]. |

Verified details from the osy clone (`references/priorart-osy-viogpu-kmd/viogpu/`):

- The KMD is about **16k lines**: `viogpu3d/` 13.8k plus `common/`. Largest files: `viogpu_adapter.cpp` 4021, `viogpu_vidpn.cpp` 3120, `common/viogpu_queue.cpp` 2258, `driver.cpp` 1899, `viogpu_allocation.cpp` 1781, `viogpu_command.cpp` 1407. Total measured: 23,706 lines including shared/common and project files.
- It implements `DxgkDdiSubmitCommand` **and** `DxgkDdiSubmitCommandVirtual` (`viogpu3d/driver.h:151,181`; `viogpu_command.cpp:1107-1172`). It uses `DxgkDdiEscape` for context init and resource info (`viogpu_device.cpp:92`, `viogpu_allocation.cpp:1427`).
- INF (`viogpu3d/viogpu3d.inx:104-106`):
  - `UserModeDriverName = "<>", neptune_umd.dll, neptune_umd.dll, neptune_umd.dll`. The D3D9 slot `<>` makes the D3D9 runtime use **D3D9On12** on top of the D3D12 UMD (BUILDING.md: "Any other value ... either fails device creation or hides the adapter").
  - `VulkanDriverName = virtio_icd.json`.
- ARM64X recipe (BUILDING.md §ARM64X):
  - Build the UMD twice (native arm64 and `/arm64EC`), then stitch them behind a code-less ARM64X forwarder with `make-arm64x.bat`, which lives in the Mesa fork at `src/virtio/neptune/triton/tools/`.
  - A WOW64 x86 UMD is registered via `UserModeDriverNameWow`.
  - The UMD file version must equal the INF `DriverVer`.
- **Venus on Windows** (`osy/virtio-win-mesa:src/virtio/vulkan/vn_renderer_virtgpu_win32.c`, 2276 lines, fetched to scratchpad):
  - Two modes: (a) loaded inside the D3D UMD with `VkD3DDDICallbacks` (uses `pfnEscapeCb`, `pfnQueryAdapterInfoCb`); (b) a **standalone Vulkan ICD** that `GetProcAddress`es the gdi32 thunks `D3DKMTQueryAdapterInfo/Escape/Render/SignalSynchronizationObject2/CreateContext/CreateAllocation/Lock/Unlock/OpenResource/CreateDevice/OpenAdapterFromHdc/CloseAdapter` (lines ~1340-1400).
  - It emulates drm_syncobj timeline semantics on top of them (comment at lines ~45-60).
  - **This is the best existing template for panvk's Windows backend.** [I]

Effort data points:

- **Neptune** (Linux-guest version) [V blog]:
  - Mesa guest driver: 13,529 hand-written lines / 256 commits.
  - Host renderserver: 9,447 lines.
  - Protocol codegen: ~10k lines.
  - DXVK fork: ~3k lines.
  - Timeline: 8 March to 16 May 2026, one developer, heavy AI assistance.
  - Lesson quoted: "Public WDDM documentation is opaque"; debugging across user/kernel and guest/host "dominated the effort".
- **viogpu3d (max8rr8):** one developer, about 1–2 months to a demo in 2023, then stalled on robustness. [I from PR timeline]
- **Overall [I]:** across these projects, getting to "DWM + a D3D/Vulkan demo" took one skilled person about 2–6 months. Getting to "runs real games without crashing" took 1–3 years and is still unfinished. All of them had the *host GPU driver* as a known-good backend. **We would additionally be bringing up real hardware (Mali CSF firmware, MMU, power, clocks) blind.**

### 1.3 VirtualBox WDDM (VBoxWddm + VBoxDX D3D11 UMD)

- [S] VirtualBox's Windows Guest Additions include a WDDM KMD and D3D UMDs, including a D3D11 UMD for VMSVGA. It is open source but GPLv3 in VirtualBox 7.
- Triton's author studied VirtualBox's DDI prototype list but did not reuse its code because of "GPLv3/MIT/LGPLv2 license incompatibility" [V Triton blog]. The Neptune blog calls it "one of the few open-source Direct3D references".
- License consideration [I]: GPLv3 is incompatible with Mesa's MIT licence and with a future MIT/Apache KMD. **Use it as a reference only.**

### 1.4 VMware SVGA WDDM

- [S] Proprietary. Mesa's `svga` Gallium driver and the (now removed) `d3d10umd` came out of VMware's Windows guest stack (Jose Fonseca) [S Phoronix]. It shows that a commercial Gallium-based WDDM UMD has existed; nothing reusable remains upstream.

### 1.5 ReactOS WDDM

- [S] https://reactos.org/blogs/investigating-wddm/ (2025): a deliberately minimal re-implementation of dxgkrnl that loads WDDM miniports, queries VidPn modes and drives CDD. A Mar 2026 report says progress was made with proprietary Intel/NVIDIA/AMD drivers.
- Relevance [I]: none as code (it is the *other side* of the DDI). It is useful as a reference for DDI call ordering and as an independent description of what dxgkrnl expects from a miniport.

### 1.6 Asahi (AGX) / freedreno-turnip on Windows

- [V/S] No Windows port of Asahi, Honeykrisp, freedreno or turnip exists or is being attempted, as far as searches show. On Windows on Snapdragon, Qualcomm ships proprietary Adreno D3D11/D3D12/Vulkan/OpenCL drivers.
- Mesa MR !29627 ("turnip, wsi: Allow building without drm/msm") targets Termux/Android KGSL, not Windows [S].
- Turnip's **KGSL backend** (merged 2020) is still a precedent [I]: a Mesa Vulkan driver gained a second, non-DRM kernel backend. That is the same idea as a panvk Windows backend.

### 1.7 Mesa on Windows: what Microsoft contributed

[V] via Mesa tree listing: `src/microsoft/` = `clc`, `compiler` (NIR→DXIL), `spirv_to_dxil`, `vulkan` (Dozen/dzn), `ci`. Also `src/gallium/drivers/d3d12` (GLOn12) and `src/gallium/winsys/d3d12/wgl`. Microsoft also did most of the MSVC/Windows build plumbing in Mesa. [I/S]

- **WGL target** (`src/gallium/targets/wgl/meson.build`) [V]: `libgallium_wgl` links `driver_swrast, driver_d3d12, driver_zink`. **So Zink as a WGL OpenGL ICD on Windows is an upstream, supported configuration.** WGL Kopper was merged in 2022 [S https://www.phoronix.com/news/Mesa-Windows-Kopper-Dzn-Dyn]. Since Mesa 24.1, Zink and d3d12 are built by default on Windows [S].
- **Vulkan WSI on Windows** (`src/vulkan/wsi/wsi_common_win32.cpp`, upstream main, 1161 lines) [V], two present paths:
  - DXGI swapchain, only if the driver provides `win32.get_d3d12_command_queue`, i.e. Dozen (line 188, 420, 858-867).
  - Otherwise a **CPU path**: `memcpy` each row from `image->base.cpu_map` into a DIB and `StretchBlt` to the window DC (lines 841-851).
  - **So panvk on Windows gets working-but-slow presentation for free via the CPU path**: Mali renders to linear host-visible memory, the CPU copies it, and GDI blits. [I] At 1080p, about 8 MB per frame is copied, which is likely OK for 30–60 fps on A76 cores but wastes bandwidth.
- **Mesa "WDDM/dxcore" bits** [I]: the "dxcore" pieces are for WSL2 (D3D12 on Linux via `/dev/dxg`), which is the reverse direction. They are not relevant except as proof that Mesa can link against `d3dkmthk.h`-style interfaces. Microsoft's own Windows GPU drivers are not in Mesa.
- **Mesa d3d10umd** (VMware/Fonseca, 2021): a Gallium frontend implementing the WDDM D3D10 UMD DDI, i.e. "Mesa as a real D3D UMD". It was removed in Mesa 26.3 (Oct 2026; ~12.7k LOC; TGSI-only) [S]. It survives in the Yttrium and osy forks [V tree]. **If we want a native D3D10/11 UMD on Gallium (panfrost or zink), we inherit a fork, or port it to NIR, which is roughly the reason it was deleted.** [I]

### 1.8 Moore Threads, Intel Arc: commercial newcomers (effort calibration)

- **Moore Threads MTT S80** (2022→): a from-scratch Windows driver by a funded company. At launch it supported "dozens" of games. DX11 came later, initially without tessellation. DX12 was limited to FL 11_1 years later [S techpowerup/igorslab/noticias3d]. Lesson [I]: even a well-funded vendor with hundreds of engineers took 2+ years to get broad D3D11 game compatibility.
- **Intel Arc / Xe** (2022): shipped **without native D3D9** and relied on Microsoft's **D3D9On12**, later going hybrid [S Intel support page via hothardware/guru3d]. Lesson [I]: mapping layers on top of a D3D12 UMD are an accepted commercial strategy.
- **Qualcomm Adreno on Windows** [S]: D3D11 + D3D12 native, D3D9 via D3D9On12. Vulkan was initially via Dozen in the Compatibility Pack, then a native Vulkan ICD around 2025. That native ICD was **ARM64-only**, so x64 DXVK did not work (see §3.4).

---

## 2. Official Arm / MediaTek / Samsung Mali drivers on Windows

- **None shipped, ever, as far as I can find.** [S, multiple searches]
  - Windows RT and Windows Phone devices were all Tegra 3 or Qualcomm Adreno.
  - Windows 10 IoT Core and IoT Enterprise Arm BSPs were Qualcomm (DragonBoard 410c, Adreno), NXP i.MX (Vivante GC7000, D3D11 driver co-developed with VeriSilicon [S NXP]) and Raspberry Pi (no GPU).
  - No Mali-based Windows device has shipped.
- **Arm [S/V]:**
  - 2022: Arm executives had "no immediate plans to release a Windows 11 hardware driver for the Immortalis line" ([V] https://www.pcworld.com/article/796784/, 2022-06-29).
  - 2024–2025: Arm careers postings (Trondheim/Raanana):
    - "Senior Graphics Software Engineer" (posted 2024-10-24): "developing the DirectX 12 driver for the latest Mali GPU".
    - "Principal DX Software Architect": "a new DX12 driver stack on Mali GPUs".
    - "Staff Software Engineer": "Direct3D 11 and 12 drivers for the latest Mali GPUs".
    - "Software Team Lead" (2025-02-12).
    - "Graduate Graphics Software Engineer, Direct3D Team: Advance Direct3D drivers for Windows on Arm".

    All of this comes from search-snippet text only. The pages now 404, so it is unverified in full.
  - Inference [I]: Arm is (or was) building a closed D3D11/D3D12 WDDM driver, likely for 5th-gen Mali / Mali G1 aimed at MediaTek (and possibly other) Windows on Arm SoCs after Qualcomm's exclusivity ended. **It is very unlikely to support Valhall G610 or to be redistributable for RK3588.** It might eventually make a "real" WDDM Mali driver exist to compare against (behaviour, INF layout).
- **MediaTek [S]:** in 2022 a MediaTek representative said they "need DirectX drivers for the Mali GPU from Arm" for Windows (Android Authority; page 403 to me). Reuters in 2024 reported a MediaTek WoA chip with Arm Cortex + Mali for late 2025. The separate MediaTek×NVIDIA N1/N1X uses an NVIDIA GPU, not Mali.
- **Samsung Exynos:** Exynos 2200+ uses AMD RDNA (Xclipse), not Mali. No Exynos Windows product shipped. Nothing reusable.
- **Reusable from vendors:** nothing binary. Arm's public Mali documentation is limited. Practically useful public material:
  - Arm's GPL **kbase** Linux driver (CSF firmware interface, power/MMU sequences). Note the licence: GPLv2, so it can be read as hardware documentation, but not copied into a non-GPL Windows driver without care.
  - **panthor** (GPLv2/MIT dual for the uAPI header).
  - Mesa's panfrost/panvk (MIT).
  - Community projects already run panvk on kbase (e.g. https://github.com/whaltermc/panvk) [S]. This shows the user-mode stack is portable across kernel interfaces.

---

## 3. Translation-layer landscape on Windows ARM64

### 3.1 Given only a Vulkan driver (panvk on Windows)

Feature reality of panvk on Valhall v10 (G610) as of Mesa main 2026-10 ([V] `src/panfrost/vulkan/panvk_vX_physical_device.c`, fetched; [V] `docs/features.txt`):

- Vulkan 1.4 complete for `panvk/v10+` (features.txt:523). G610 has a conformant Vulkan 1.4 submission (docs/drivers/panfrost.rst:92).
- Notable **false** features:
  - `geometryShader` (line 315)
  - `tessellationShader` (316)
  - `fillModeNonSolid` (324)
  - `depthBounds` (325)
  - `multiViewport` (329)
  - `shaderClipDistance` / `shaderCullDistance` (352-353)
  - `shaderFloat64` (354)
  - `pipelineStatisticsQuery` (335)
  - `variableMultisampleRate` (368)
- `textureCompressionBC` depends on the SoC's enabled formats (lines 291-299). [I] Unknown whether RK3588 fuses BC on; needs checking on hardware.
- GS work in flight:
  - Mesa MR !38401 "Very Draft! Panvk Geometry Shaders" (gfxstrand, 2025-11).
  - MR !44118 "panvk/csf: start implementing experimental GS support" (Olivia Lee, updated 2026-10-29): "based on libpoly ... XFB, multidraw, queries, multiview ... future MRs ... GS support will only be advertised if the `panvk_enable_gs_xfb` driconf option is enabled."
  - `src/poly` (shared GS/tess-emulation library from Asahi/KosmicKrisp) has recent panvk-targeted commits (MR !41192 "poly: GS/XFB shader lowering changes in preparation for panvk").
  - No tessellation MR found.

What a user would get:

| API | Path | Expected coverage on G610 (today → with GS/tess) | Confidence |
|---|---|---|---|
| Vulkan | panvk ICD (registered via `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` or adapter `VulkanDriverName`, [V] Vulkan-Loader `docs/LoaderDriverInterface.md` "Driver Discovery on Windows") | 1.4 (same as Linux, minus WSI-related extensions) | high [I] |
| OpenGL | Zink via Mesa WGL ICD (`gallium_wgl` links zink [V]) | Zink exposes GL by Vulkan features. GL 3.2 needs GS, GL 4.0 needs tess, and clip distance is needed for GL 3.0 `gl_ClipDistance` (Zink can lower). Expect **~GL 3.1 / GLES 3.1 now; GL 4.x after GS+tess+clip land** | medium [I] (verify with `zink` requirements tool / `src/gallium/drivers/zink/zink_screen.c`) |
| OpenGL (alt) | native **panfrost Gallium** in WGL (needs a new Windows winsys) | GL 3.1 / GLES 3.1 (features.txt shows panfrost in GL 3.1 but not 3.2) | high [V features.txt] |
| D3D9/10/11 | **DXVK** | **Not usable**: DXVK requires `geometryShader`, `fillModeNonSolid`, `multiViewport`, `shaderClipDistance`, `shaderCullDistance`, `depthClamp`, `textureCompressionBC`, `robustness2.nullDescriptor`, Vulkan 1.3 core, maintenance5/6 ... ([V] `references/priorart-dxvk/src/dxvk/dxvk_device_info.cpp:847-1066`, `require=true` entries). Becomes plausible once panvk has GS/XFB/clip/multiviewport/non-solid fill. | high [V] |
| D3D9/10/11 | Gallium D3D frontend (Yttrium-style d3d10umd + Nine-based D3D9) on zink or panfrost, as a **real WDDM UMD** | D3D10 needs GS too; D3D9 is feasible early (no GS) | medium [I] |
| D3D12 | vkd3d-proton | Starts at FL 11_0 (`libs/vkd3d/device.c:9699`) and requires robustness2/nullDescriptor, push descriptors, maintenance5/6, samplerMirrorClampToEdge (lines 2635-2670) plus D3D12 resource-binding-tier descriptor counts. **Not realistic on panvk for a long time.** | medium-high [V/I] |
| OpenCL | rusticl on panfrost (Mesa) or clvk on Vulkan | possible later | low priority [I] |

Windows-specific caveats:

- **No system D3D without WDDM.** DXVK only works as app-local `d3d11.dll`/`dxgi.dll` replacements, and anti-cheat or launchers may object. DXGI swapchains need a WDDM adapter. [I]
- **OpenGL ICD discovery:** `opengl32.dll` picks the ICD from the WDDM adapter's `OpenGLDriverName` registry value. With a non-WDDM (KMDF) device, the display adapter is MSBDD and has no ICD, so system-wide GL would fall back to GDI GL 1.1 or the Compatibility Pack. A Zink `opengl32.dll` would have to be dropped next to the app (as mesa-dist-win does). [I; ICD lookup mechanism from general knowledge, not re-verified]
- **x64 emulation (Prism):** most games are x64. An x64 process can only load x64 or ARM64EC code, so a native-ARM64 Vulkan ICD is invisible to x64 DXVK/vkd3d-proton/apps.
  - [V] DXVK issue #5125 (https://github.com/doitsujin/dxvk/issues/5125): on Snapdragon X, DXVK did nothing; the reporter asked Qualcomm "about shipping an x64 ICD and ARM64EC loader", and managed to build an ARM64EC DXVK.
  - [V] osy BUILDING.md: "a native-arm64 neptune_umd.dll can be loaded by native arm64 apps but not by x64-emulated Direct3D apps — those need an ARM64EC view".
  - [V] vkd3d-proton ships `build-arm64x.txt` (arm64ec-w64-mingw32-gcc, `-marm64x`) and its CHANGELOG says "aarch64 builds are supported" (Steam Frame).
  - [S] LunarG ships ARM64X loader binaries (https://lunarg.com/wp-content/uploads/2024/08/Vulkan-SDK-for-Windows-11-ARM64.pdf).
  - **So panvk must eventually be built as ARM64X (arm64 + arm64ec), and WOW64 x86 (32-bit games) needs a separate x86 build.** [I] Mesa with arm64ec is non-trivial: no LLVM needed for panvk (Bifrost/Valhall compiler is in-tree), but mingw arm64ec support in llvm-mingw needs checking.

### 3.2 Given a D3D12 driver (WDDM UMD + KMD)

| API | Path | Notes |
|---|---|---|
| D3D12 | native | FL 11_0 minimum for D3D12 apps; requires WDDM 2.0+ GPU VA. [I/general] |
| D3D11 / D3D10.x | OS **11on12** as the D3D11 UMD (`D3D11_UMD_MODE_SYSTEM11ON12` in `d3d10umddi.h`) | [V] https://ntdoc.m417z.com/d3d11_umd_mode. That an INF can rely on it entirely without shipping any D3D11 UMD is [I]; osy registers its own UMD for 10/11. |
| D3D9 | **D3D9On12** (INF D3D9 slot `<>`) | [V] osy BUILDING.md; precedent: Intel Arc [S], Qualcomm [S] |
| OpenGL | GLOn12 (Mesa d3d12 Gallium) via the "OpenCL, OpenGL and Vulkan Compatibility Pack" | Mesa main: d3d12 in the GL 4.6 line [V features.txt:229]. Store text says "OpenGL 3.3 and earlier" [S]. |
| OpenCL | CLOn12 | Store text: "OpenCL 3.0 and earlier" [S] |
| Vulkan | Dozen (dzn) | features.txt lists dzn only under "Vulkan 1.0 -- all DONE" [V]; Store text says "Vulkan 1.2 and earlier" [S]; performance and compatibility-oriented [S] |

- **This is the coverage-maximising path, and it is the one Arm itself appears to be on.**
- **The catch [I]:** a D3D12 UMD is the hardest component in the whole space. It needs:
  - A DXIL→Mali compiler. Mesa has DXIL→NIR via `spirv_to_dxil`'s sibling? No: Mesa has **NIR→DXIL**, not DXIL→NIR. A DXIL front-end would come from vkd3d-proton's dxil-spirv → SPIR-V → NIR, or from Microsoft's open-source DXC/LLVM.
  - Full D3D12 binding model and residency.
  - Enhanced barriers.
  - FL 11_0 with GS/tess.
  - Plus the KMD having to support WDDM 2.x GPU VA, monitored fences and (on modern Windows) hardware scheduling or emulated paths.
- `cosumd12` (Microsoft sample) and Microsoft's `D3D12TranslationLayer`/`D3D11On12` (open source) are the only public references for D3D12 UMD structure. anonymix007 has forks of both (github.com/anonymix007/D3D11On12, /D3D12TranslationLayer) [V repo list].

### 3.3 "Vulkan first, D3D via a Mesa Gallium UMD" (the Yttrium model)

- Yttrium shows a third path: **a WDDM D3D9/10/11 UMD built on Gallium, running on Zink (on Venus)** [V README + tree]. For us, the same UMD could sit on **Zink-on-panvk** or directly on **panfrost Gallium**. This gives system-level D3D9 (no GS needed) early, without a DXIL compiler, using DXBC→TGSI→NIR (fork-maintained).
- Risk [I]: it is built on removed upstream code (d3d10umd, TGSI) and an alpha fork.

### 3.4 Summary of the matrix

- **Vulkan-only driver:** Vulkan 1.4, GL ~3.1→4.x via Zink as panvk grows, D3D via app-local DXVK only once GS/tess/etc. land. No DWM acceleration, no system D3D. Good for developers and emulators (RPCS3/Dolphin/Yuzu-likes); poor for "Windows games".
- **WDDM + Vulkan ICD + Gallium D3D UMD:** adds system D3D9 (and D3D10/11 after GS), DWM, and proper presentation.
- **WDDM + D3D12 UMD:** maximum coverage via Microsoft mapping layers, at maximum effort.

---

## 4. Hardest problems and realistic milestone sequencing

### 4.1 Hardest problems (roughly in order of risk) [I unless marked]

1. **Blind hardware bring-up on Windows with no Mali hardware on the dev machine.** The panthor/CSF sequence must be reproduced in a Windows KMD:
   - power domains/regulators, clocks, resets
   - MCU firmware load (`mali_csffw.bin`)
   - GPU MMU/AS setup
   - CSF global/CSG/CS interfaces
   - doorbells, IRQs

   RK3588 power and clock control is normally done by Linux drivers (SCMI clocks, PM domains). Under Windows/ACPI it must come from edk2-rk3588 firmware (already powered on?), ACPI `_PS0`/`_PR0`, or the KMD poking CRU/PMU registers directly.

   *Whether edk2-rk3588 exposes the GPU in DSDT and leaves it powered is an open question.* A maintainer said "Mainline Mali GPU driver doesn't support acpi mode yet" [V edk2-rk3588 issue #184 quote]. A debugger (serial KD on UART2 per the Rockchip-Windows-Drivers table) is essential.
2. **WDDM semantics vs panthor semantics.**
   - WDDM owns memory residency/paging, scheduling (DMA buffers, fences, preemption, TDR), and allocations. panthor/CSF has *user-mode queues* in firmware-scheduled ring buffers.
   - Mapping CSF's user-submitted rings onto WDDM is uncharted. Options include WDDM 2.x "hardware scheduling" / user-mode submission (WDDM 2.7+/3.x HWS, user-mode queues in recent WDDM) or a "fake" DMA-buffer path where `SubmitCommandVirtual` just rings a doorbell.
   - Preemption: viogpu3d had to disable it system-wide [V PR #943]. TDR must work or the system will reset the driver.
3. **No public documentation of the "unwritten rules" of WDDM.** This was stressed independently by anonymix007 ("writing code is not the bottleneck") and osy ("Public WDDM documentation is opaque") [V]. Expect weeks of WinDbg on things like VidPn, DWM present paths, the D3D9On12 INF slot quirk, and the UMD version-equals-DriverVer rule.
4. **Toolchain / signing.**
   - Kernel driver for ARM64 without `ntoskrnl.lib`/`displib.lib`/`dxgkrnl` import libs in llvm-mingw. anonymix007 builds WDDM KMDs **on Linux using clang-cl + the EWDK via a case-insensitive VFS overlay** [V README, https://github.com/tmp64/clang-cl-linux/]. This is the obvious solution for our machine; the EWDK licence must be accepted by the user.
   - Signing: test-signing (`bcdedit /set testsigning on`) on dev boards; no WHQL path for a hobby driver. Secure Boot must be off.
5. **Presentation and display.** On RK3588 the only display today is the UEFI GOP framebuffer via MSBDD (1080p60, one head) [V Rockchip-Windows-Drivers]. Options:
   - (a) render-only WDDM + MSBDD with cross-adapter copies (DWM handles it slowly; render-only cannot be the primary adapter on client SKUs [S MS docs]).
   - (b) full WDDM KMD that also owns the GOP framebuffer, like BDD/KMDOD do and as ROS does in non-render-only mode.
   - (c) later, a real VOP2 display driver.

   Option (b) is the sweet spot [I]: the GOP framebuffer is in DRAM, so Mali can render or blit straight into it.
6. **Feature gaps in panvk** (GS, tess, clip/cull, multiViewport, non-solid fill) block DXVK and limit Zink. This is upstream Mesa work, progressing (GS via libpoly) but not under our control.
7. **ARM64X / ARM64EC / WOW64 packaging** for x64- and x86-emulated apps.
8. **Upstreamability.** A Windows backend for panvk has to fit Mesa's kmod abstraction (`src/panfrost/lib/kmod/` already has `panfrost_kmod.c`, `panthor_kmod.c` and a `virtio/` backend [V tree], with ops `dev_create/bo_alloc/bo_mmap/bo_wait/vm_create/...` [V `pan_kmod.h:~460-513`]). Submission and sync in panvk still use DRM ioctls and syncobjs directly (owned by another researcher; not verified here). Mesa has precedent for Windows backends (Venus win32 lives in a fork, not upstream).

### 4.2 Milestone sequencing [I]

Each milestone is independently demoable. Effort figures are for one experienced developer with AI help, after the hardware is in hand.

- **M0 (no hardware needed, weeks):**
  - Toolchain: build an ARM64 KMDF and a WDDM KMD skeleton (ROS/KMDOD/viogpu3d-shaped) on Linux with clang-cl + EWDK.
  - Build panvk for `aarch64-w64-mingw32` against a stub kmod.
  - Design the escape/IOCTL ABI (panthor-like).
  - Optional: run a Windows ARM64 VM to test the WDDM skeleton (`qemu-system-aarch64` is not installed here).
- **M1 (hardware, 1–3 months): "GPU alive".**
  - KMDF driver (simpler than WDDM for bring-up) that maps GPU registers from ACPI/hard-coded addresses, powers it, reads `GPU_ID` (arch major 10 / G610 product id; exact value to be checked against panthor's `panthor_hw.c`), loads CSF firmware, gets a GLB interface response, sets up MMU AS, and runs a trivial CS job (e.g. a `WRITE`/`FINISH` instruction stream).
  - Debug over serial KD.
- **M2 (1–2 months): "Vulkan compute".** panvk with a Windows kmod backend talking to the M1 driver via IOCTLs; `vulkaninfo`, then a compute dispatch (e.g. a vkcube-less compute test, or a subset of dEQP-VK compute).
- **M3 (1–2 months): "Vulkan graphics on screen, slow".** vkcube via the upstream Mesa win32 WSI CPU/GDI path (§1.7). At this point Zink GL also mostly works (GL ≤3.1) via app-local `opengl32.dll`. **This is the first user-visible milestone.**
- **M4 (3–6 months): "WDDM".**
  - Move the KMD to WDDM 2.x (render + GOP-framebuffer display): allocations, GPU VA, contexts/submission mapped to CSF, monitored fences, TDR, then Vulkan ICD registration via `VulkanDriverName` and D3DKMT-based kmod (Venus-win32 pattern).
  - DWM-accelerated desktop requires a D3D UMD (DWM uses D3D11). **This is the crux** [I]: DWM acceleration needs either a D3D11 UMD (Gallium d3d10umd fork) or a D3D12 UMD with system 11on12.
- **M5 (3–6 months): "System GL + D3D9".** Zink as the registered `OpenGLDriverName` ICD. A Gallium D3D9 UMD (Yttrium-style), registered as the D3D9 UMD. ARM64X builds.
- **M6 (open-ended): "Games".**
  - D3D10/11 via Gallium UMD or DXVK once panvk gets GS/tess (track MR !44118 and successors).
  - Performance: tiler heap growth, power management, DVFS (CPU DVFS doesn't work on Windows RK3588 today either [V]).
- **M7 (optional, multi-year): "D3D12 UMD".** Only if the project grows to several people. Otherwise rely on Arm possibly shipping an official driver for newer Mali.

**Overall realism [I]:**
- M1–M3: 4–8 months part- to full-time; achievable.
- M4–M5: another 6–12 months.
- "Most D3D11 games run" is multi-year and depends on upstream panvk feature work.

### 4.3 Alternative strategies considered (and verdicts) [I]

| Strategy | Verdict |
|---|---|
| Straight port of Linux panfrost/panthor kernel code into a Windows driver | Reuse the *logic* (CSF init, MMU, scheduling), not the code: GPLv2, and Linux DRM/GEM/dma-fence/drm_sched don't exist on Windows. |
| KMDF-only driver + Vulkan ICD, never WDDM | Fastest to first pixels (M3); dead end for D3D/DWM/system GL. Good as a bring-up stage only. |
| WDDM KMD + panvk ICD + Zink + Gallium D3D9/10/11 UMD (Yttrium model) | **Recommended long-term architecture**: maximises Mesa reuse and has working precedent on Windows (incl. ARM64X). |
| WDDM KMD + own D3D12 UMD + MS mapping layers | Best coverage, worst effort; likely what Arm does. Defer. |
| Port Arm's kbase driver instead of panthor semantics | kbase is GPL too and its uAPI is larger and unstable. panvk-on-kbase exists [S], but panthor's uAPI is smaller, documented and upstream; keep panthor-like. |
| Paravirtual approach (Linux VM on the board owning the GPU, Windows talking virtio-gpu/Venus/Neptune to it) | Interesting fallback [I]: reuses the entire existing viogpu3d/Yttrium/Neptune Windows stack unchanged plus Linux panthor+panvk. But Windows would need to run under a hypervisor on RK3588 (KVM host on Linux with a Windows ARM64 guest). Works in principle; heavy and slow, and not "a Windows driver". Worth noting as a "plan B to get *something* accelerated". |

---

## 5. Open questions

1. Does edk2-rk3588's ACPI (DSDT) describe the Mali GPU (MMIO, 3 IRQs: job/mmu/gpu), and does firmware power it on? If not, the KMD must do PMU/CRU work or the DSDT must be extended (we can patch edk2-rk3588 ourselves).
2. Is RK3588's G610 fused with BC texture support (matters for DXVK/D3D)? Check `TEXTURE_FEATURES` registers on hardware or Linux `panthor` output.
3. Can llvm-mingw produce arm64ec/ARM64X for Mesa, or do we need MSVC/clang-cl + EWDK for that too?
4. What exactly does a WDDM KMD need to do to support CSF-style user-mode submission (WDDM 3.x user-mode queues / HWS) on ARM64 Windows 11 24H2+? Not researched in depth here.
5. Exact Zink GL version on panvk today (run `zink` on panvk on Linux, which needs hardware), and whether Zink can lower clip distance / emulate GS-less GL 3.2 paths.
6. Licensing choice for our KMD (MIT to stay compatible with Mesa and Microsoft samples). Avoid copying GPL code from panthor, kbase or VirtualBox, and AGPL code from anonymix007.
7. Has the Arm D3D12 Mali driver shipped on any device yet (late 2026)? None was found.

## 6. URL index

- https://github.com/worproject/RPi-Windows-Drivers
- https://github.com/worproject/Rockchip-Windows-Drivers (+ issue #54)
- https://github.com/edk2-porting/edk2-rk3588/issues/184
- https://github.com/microsoft/graphics-driver-samples
- https://github.com/TheMindVirus/VC4WDDM2
- https://github.com/virtio-win/kvm-guest-drivers-windows/pull/943
- https://github.com/osy/kvm-guest-drivers-windows (branch neptune), https://github.com/osy/virtio-win-mesa (branch neptune)
- https://blog.getutm.app/2026/introducing-neptune-direct3d-virtualization-for-qemu/
- https://blog.getutm.app/2026/introducing-triton-directx-11-driver-for-qemu/
- https://github.com/arehnman/yttrium-virtio-gpu, https://github.com/arehnman/virtio-win-mesa (yttrium-experimental)
- https://github.com/anonymix007/virtio-drivers-windows-rs, https://github.com/anonymix007/virtio-d3d11, https://github.com/anonymix007/kvm-guest-drivers-windows-venus
- https://github.com/tmp64/clang-cl-linux/
- https://www.phoronix.com/news/Mesa-Drops-Direct3D-10-UMD
- https://www.phoronix.com/news/Mesa-Windows-Kopper-Dzn-Dyn
- https://reactos.org/blogs/investigating-wddm/
- https://github.com/doitsujin/dxvk/issues/5125
- https://github.com/HansKristian-Work/vkd3d-proton (build-arm64x.txt, CHANGELOG)
- https://raw.githubusercontent.com/KhronosGroup/Vulkan-Loader/main/docs/LoaderDriverInterface.md
- https://lunarg.com/wp-content/uploads/2024/08/Vulkan-SDK-for-Windows-11-ARM64.pdf
- https://ntdoc.m417z.com/d3d11_umd_mode
- https://devblogs.microsoft.com/directx/announcing-the-opencl-and-opengl-compatibility-pack-for-windows-10-on-arm/
- https://apps.microsoft.com/detail/9nqpsl29bfff (Compatibility Pack)
- https://learn.microsoft.com/windows-hardware/drivers/display/using-cross-adapter-resources-in-a-hybrid-system
- https://www.pcworld.com/article/796784/arm-promises-powerful-new-pcs-with-cortex-x3-immortalis-gpus.html
- https://careers.arm.com (D3D12 Mali postings, now 404)
- Mesa MRs: https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/44118, /38401, /41192, /29627
- https://github.com/whaltermc/panvk
