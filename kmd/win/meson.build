# SPDX-License-Identifier: MIT
# KMDF glue sources for panwin.sys (included from kmd/meson.build).

pw_win_sources = files(
  'pw_fmt.c',
  'pw_os_win.c',
  'pw_win_driver.c',
  'pw_win_ioctl.c',
  'pw_win_plat.c',
  'pw_win_user.c',
)
pw_win_stub_core_sources = files('pw_core_stub.c')
pw_win_inc = include_directories('.')

if get_option('host_tests')
  test_fmt = executable('test_fmt', 'tests/test_fmt.c', 'pw_fmt.c',
    include_directories: pw_win_inc,
    c_args: ['-Wno-format'],
    native: true,
    build_by_default: true)
  test('pw_fmt (host)', test_fmt, suite: 'host')
  test_plat = executable('test_plat', 'tests/test_plat.c', 'pw_win_plat.c',
    include_directories: pw_win_inc,
    native: true,
    build_by_default: true)
  test('pw_win_plat (host)', test_plat, suite: 'host')
endif

# Static INF checks: G31 sections identical in both INFs, RK3588 install path unchanged.
test('INF check', python,
  args: ['-I', files('tests/check_inf.py'), files('panwin.inf'), files('panwin-zero3.inf')],
  suite: 'host')

install_data('panwin.inf', 'panwin-zero3.inf', install_dir: get_option('prefix'))
