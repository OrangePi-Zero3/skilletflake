# SPDX-License-Identifier: MIT
# panwin-info: bring-up tool. Consumes libdrm-win through its installed libdrm.pc, exactly like
# the Mesa cross build does (configure with -Dpkg_config_path=<prefix>/lib/pkgconfig, see
# userspace/libdrm-win/build.sh).
project('panwin-info', 'c',
  version : '0.1.0',
  license : 'MIT',
  meson_version : '>= 0.60',
  default_options : ['c_std=c11', 'warning_level=2', 'buildtype=debugoptimized'])

dep_libdrm = dependency('libdrm', version : '>= 2.4.109')

executable('panwin-info', 'panwin-info.c',
  dependencies : dep_libdrm,
  c_args : ['-mno-ms-bitfields', '-D_WIN32_WINNT=0x0A00'],
  install : true)
