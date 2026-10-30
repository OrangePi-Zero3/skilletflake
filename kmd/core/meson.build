# SPDX-License-Identifier: MIT
# OS-neutral Mali core (CSF + job-manager backends). Consumed by kmd/win (subdir('core'))
# and built freestanding.
pw_core_sources = files(
  'pw_bo.c',
  'pw_device.c',
  'pw_file.c',
  'pw_fw.c',
  'pw_gpu.c',
  'pw_handle.c',
  'pw_heap.c',
  'pw_irq.c',
  'pw_jm_hw.c',
  'pw_jm_mem.c',
  'pw_jm_sched.c',
  'pw_mmu.c',
  'pw_sched.c',
  'pw_sync.c',
  'pw_util.c',
)

pw_core_inc = include_directories(
  '.',
  '../compat-km',
  '../../include',
  '../../include/drm-uapi',
  '../../include/compat',
)
