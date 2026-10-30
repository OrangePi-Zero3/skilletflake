# SPDX-License-Identifier: MIT
# OS-neutral Mali CSF core. Consumed by kmd/win (subdir('core')) and built freestanding.
pw_core_sources = files(
  'pw_device.c',
  'pw_fw.c',
  'pw_gpu.c',
  'pw_irq.c',
  'pw_mmu.c',
  'pw_util.c',
)

pw_core_inc = include_directories(
  '.',
  '../compat-km',
  '../../include',
  '../../include/drm-uapi',
  '../../include/compat',
)
