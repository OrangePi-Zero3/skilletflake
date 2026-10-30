# SPDX-License-Identifier: MIT
#
# libdrm-win: libdrm-compatible libdrm.dll for Windows (ARM64 / x86_64) over panwin.sys.
# The version tracks the upstream libdrm whose xf86drm.h is shipped (Mesa needs >= 2.4.109).

project('libdrm-win', 'c',
  version : '2.4.134',
  license : 'MIT',
  meson_version : '>= 0.60',
  default_options : ['c_std=c11', 'warning_level=2', 'buildtype=debugoptimized'])

if host_machine.system() != 'windows'
  error('libdrm-win only targets Windows (use cross/<arch>-w64-mingw32.ini)')
endif

cc = meson.get_compiler('c')

# The shared contracts live in the panfrost-win repo root (read-only for this component).
repo_inc = include_directories('../../include')
inc = include_directories('include', 'src', '../../include/drm-uapi', '../../include/compat')

c_args = [
  '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-DUNICODE', '-D_UNICODE',
  '-DWIN32_LEAN_AND_MEAN',
  # Mesa is built with -mno-ms-bitfields; keep identical struct layout rules.
  '-mno-ms-bitfields',
  '-Wno-unused-parameter',
]

with_fake = get_option('fake_kernel')
if with_fake
  c_args += '-DPANWIN_HAVE_FAKE_KERNEL'
endif

srcs = files('src/xf86drm_win.c', 'src/fd.c', 'src/errno_map.c', 'src/transport_win32.c')
if with_fake
  srcs += files('src/fake_kernel.c')
endif

win_deps = [cc.find_library('setupapi'), cc.find_library('ntdll')]

# ---- optional in-process "core" transport: the real kmd/core on the mock G610 ----
with_core = get_option('core_transport')
link_whole_libs = []
if with_core
  if host_machine.cpu_family() != 'x86_64'
    error('-Dcore_transport=true is for x86_64 test builds (run under wine) only')
  endif
  core_fw = get_option('core_fw')
  if core_fw == ''
    core_fw = 'Z:' + (meson.project_source_root() / '..' / '..' / '..' / 'references' / 'firmware' / 'mali_csffw.bin')
  endif
  c_args += ['-DPANWIN_HAVE_CORE_TRANSPORT', '-DPANWIN_CORE_FW_DEFAULT="' + core_fw + '"']
  # The core's own source list (kmd/core/meson.build) plus the host-test mock.
  pw_core_files = []
  foreach f : ['pw_bo.c', 'pw_device.c', 'pw_file.c', 'pw_fw.c', 'pw_gpu.c', 'pw_handle.c',
               'pw_heap.c', 'pw_irq.c', 'pw_jm_hw.c', 'pw_jm_mem.c', 'pw_jm_sched.c', 'pw_mmu.c',
               'pw_sched.c', 'pw_sync.c', 'pw_util.c']
    pw_core_files += files('../../kmd/core' / f)
  endforeach
  pwcore = static_library('pwcore_mock',
    pw_core_files, files('../../kmd/tests/pw_os_mock.c', '../../kmd/tests/mock_csf.c',
                         '../../kmd/tests/mock_jm.c'),
    include_directories : include_directories('../../kmd/core', '../../kmd/tests', '../../include',
                                              '../../include/drm-uapi', '../../include/compat'),
    c_args : ['-mno-ms-bitfields', '-D_GNU_SOURCE'],
    pic : true)
  link_whole_libs += pwcore
  inc = include_directories('include', 'src', '../../include/drm-uapi', '../../include/compat',
                            '../../kmd/core', '../../kmd/tests')
  # Static winpthreads (the mock's IRQ/work threads): libdrm.dll keeps importing only system DLLs.
  win_deps += cc.find_library('winpthread', static : true)
  srcs += files('src/transport_core.c')
endif

libdrm = shared_library('drm', srcs,
  include_directories : [inc, repo_inc],
  c_args : c_args,
  dependencies : win_deps,
  vs_module_defs : 'libdrm.def',
  link_whole : link_whole_libs,
  name_prefix : 'lib',
  install : true)

idep_libdrm = declare_dependency(link_with : libdrm,
  include_directories : [include_directories('include'), repo_inc,
                         include_directories('../../include/drm-uapi', '../../include/compat')])

# ---- headers, laid out like upstream libdrm: xf86drm.h at the top, kernel headers in libdrm/ ----
install_headers('include/xf86drm.h', 'include/libdrm_win.h', 'include/panwin_transport.h',
                'include/panwin_core.h')
install_headers('../../include/drm-uapi/drm.h', '../../include/drm-uapi/drm_mode.h',
                '../../include/drm-uapi/panthor_drm.h', '../../include/drm-uapi/panfrost_drm.h',
                subdir : 'libdrm')
install_headers('../../include/compat/sys/ioccom.h', subdir : 'libdrm/sys')
install_headers('../../include/panwin/panwin_ioctl.h', subdir : 'panwin')
install_data('LICENSE', install_dir : get_option('datadir') / 'licenses' / 'libdrm-win')

pkg = import('pkgconfig')
pkg.generate(libdrm,
  name : 'libdrm',
  filebase : 'libdrm',
  description : 'Userspace interface to kernel DRM services (libdrm-win: panwin.sys backend)',
  subdirs : ['.', 'libdrm'])

# ---- tests ----
# Compile-time ioctl-number mapping checks (always built: they need no device).
test_map = executable('test_ioctl_map', 'tests/test_ioctl_map.c',
  include_directories : [inc, repo_inc], c_args : c_args, link_with : libdrm)
test('ioctl_map', test_map)

# Runtime tests against the in-process fake kernel. Executables sit next to libdrm.dll so the
# Windows loader finds it (also under wine).
if with_fake
  # Transport DLL for the PANWIN_TRANSPORT=dll:<path> loader (see include/panwin_transport.h).
  transport_dll = shared_module('panwin_test_transport', 'tests/test_transport_dll.c',
    include_directories : [inc, repo_inc], c_args : c_args, name_prefix : '')
  test_fake = executable('test_libdrm_fake', 'tests/test_libdrm_fake.c',
    include_directories : [inc, repo_inc], c_args : c_args, link_with : libdrm)
  # Under wine (cross build) the DLL's Linux path is reached through the Z: drive.
  dll_path = (meson.is_cross_build() ? 'Z:' : '') + transport_dll.full_path()
  test('libdrm_fake', test_fake, timeout : 120, depends : transport_dll,
       env : ['PANWIN_FAKE_KERNEL=1', 'WINEDEBUG=-all', 'PANWIN_TEST_TRANSPORT_DLL=' + dll_path])
endif

# Runtime test against the REAL kmd/core + mock G610 in-process (transport "core").
if with_core
  test_core = executable('test_libdrm_core', 'tests/test_libdrm_core.c',
    include_directories : [inc, repo_inc], c_args : c_args, link_with : libdrm)
  test('libdrm_core', test_core, timeout : 300,
       env : ['PANWIN_TRANSPORT=core', 'WINEDEBUG=-all'])
  # The same core on the mock Mali-G31 (Orange Pi Zero3, job manager, panfrost uAPI).
  test_core_g31 = executable('test_libdrm_core_g31', 'tests/test_libdrm_core_g31.c',
    include_directories : [inc, repo_inc], c_args : c_args, link_with : libdrm)
  test('libdrm_core_g31', test_core_g31, timeout : 300,
       env : ['PANWIN_TRANSPORT=core', 'PANWIN_CORE_MODEL=g31', 'WINEDEBUG=-all'])
  # (jm-e2e) The G31 job path below Mesa: BOs, v7 job chains, SUBMIT, sync, faults, heap, stress.
  test_jm = executable('test_libdrm_jm', 'tests/test_libdrm_jm.c',
    include_directories : [inc, repo_inc], c_args : c_args, link_with : libdrm)
  test('libdrm_jm', test_jm, timeout : 300,
       env : ['PANWIN_TRANSPORT=core', 'PANWIN_CORE_MODEL=g31', 'WINEDEBUG=-all'])
endif

summary({'fake kernel transport' : with_fake, 'core transport (kmd/core + mock)' : with_core},
        section : 'libdrm-win')
