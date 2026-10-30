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

libdrm = shared_library('drm', srcs,
  include_directories : [inc, repo_inc],
  c_args : c_args,
  dependencies : win_deps,
  vs_module_defs : 'libdrm.def',
  name_prefix : 'lib',
  install : true)

idep_libdrm = declare_dependency(link_with : libdrm,
  include_directories : [include_directories('include'), repo_inc,
                         include_directories('../../include/drm-uapi', '../../include/compat')])

# ---- headers, laid out like upstream libdrm: xf86drm.h at the top, kernel headers in libdrm/ ----
install_headers('include/xf86drm.h', 'include/libdrm_win.h')
install_headers('../../include/drm-uapi/drm.h', '../../include/drm-uapi/drm_mode.h',
                '../../include/drm-uapi/panthor_drm.h', subdir : 'libdrm')
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
  test_fake = executable('test_libdrm_fake', 'tests/test_libdrm_fake.c',
    include_directories : [inc, repo_inc], c_args : c_args, link_with : libdrm)
  test('libdrm_fake', test_fake, timeout : 120,
       env : ['PANWIN_FAKE_KERNEL=1', 'WINEDEBUG=-all'])
endif

summary({'fake kernel transport' : with_fake}, section : 'libdrm-win')
