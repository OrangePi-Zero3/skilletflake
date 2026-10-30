# SPDX-License-Identifier: MIT
#
# panwin.sys - KMDF kernel driver for Arm Mali (RK3588 Mali-G610) on Windows on ARM64.
#
# Cross build only (Linux host, llvm-mingw):
#   meson setup build-kmd kmd --cross-file toolchain/meson/aarch64-w64-kmd.ini [-Dcore=real]
#   ninja -C build-kmd            # -> build-kmd/panwin.sys (+ .pdb, .map)
#   meson test -C build-kmd       # image/import checks + host unit tests
#
# Compile flags follow the proven toolchain experiment (docs/research/windows-driver-model.md
# section 2); the image is linked by lld-link directly (MSVC-style flags) from a static
# library of all objects, entry point FxDriverEntry (toolchain/kmdf/fxstub.c).

project('panwin-kmd', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 1.1.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
    'b_staticpic=false',
    'b_pie=false',
  ])

if host_machine.system() != 'windows' or host_machine.cpu_family() != 'aarch64'
  error('panwin.sys is ARM64 Windows only: configure with --cross-file toolchain/meson/aarch64-w64-kmd.ini')
endif

cc = meson.get_compiler('c')
fs = import('fs')
toolchain = meson.project_source_root() / '..' / 'toolchain'
repo_include = meson.project_source_root() / '..' / 'include'

lld_link = find_program('lld-link')
dlltool = find_program('llvm-dlltool')
python = find_program('python3')
petool = files('../toolchain/scripts/petool.py')

# ---------------------------------------------------------------------------- headers
# Order matters: our patched ARM64 wdm.h/ntddk.h must shadow the mingw-w64 copies.
mingw_ddk = meson.get_external_property('mingw_ddk_include')
if not fs.is_dir(mingw_ddk)
  error('mingw_ddk_include (' + mingw_ddk + ') from the cross file does not exist')
endif
kernel_isystem = []
foreach d : [toolchain / 'include' / 'ddk', mingw_ddk,
             toolchain / 'include' / 'wdf' / 'kmdf' / '1.15', toolchain / 'include' / 'wdfcase']
  kernel_isystem += ['-isystem', d]
endforeach

# ------------------------------------------------------------------------ compile flags
kernel_c_args = kernel_isystem + [
  '-ffreestanding', '-fno-builtin',  # no CRT; memcpy & co. are ntoskrnl imports
  '-fno-stack-protector',            # no __security_cookie / __stack_chk_* in the kernel
  '-funwind-tables',                 # REQUIRED: -ffreestanding drops .pdata/.xdata otherwise
  '-fno-auto-import',                # no mingw pseudo-relocations (nobody would apply them)
  '-fms-extensions',                 # __try/__except (SEH -> __C_specific_handler)
  '-mno-outline-atomics',            # never call libgcc __aarch64_* helpers
  '-fno-strict-aliasing',
  '-D_KERNEL_MODE', '-DNTDDI_VERSION=0x0A000000', '-D_WIN32_WINNT=0x0A00',
  '-DPANWIN_VERSION_STRING="' + meson.project_version() + '"',
  '-DPANWIN_DEBUG_IOCTLS=' + (get_option('debug_ioctls') ? '1' : '0'),
  # WDF_DECLARE_CONTEXT_TYPE puts its const type info in a "#pragma section" .data, which clang
  # emits as a second, read-only ".data" section; put it in .rdata instead (it is never written).
  '-DWDF_TYPE_DEFAULT_SECTION_NAME=".rdata"',
  # Mesa's drm.h uses enum constants > INT_MAX (DRM_IOCTL_*), a C11 extension.
  '-Wno-pedantic',
]
if get_option('debug')
  kernel_c_args += ['-gcodeview']   # CodeView -> lld-link writes a PDB for WinDbg
endif
foreach a : ['-Wmissing-prototypes', '-Wstrict-prototypes', '-Wshadow', '-Wvla',
             '-Wframe-larger-than=2048']
  if cc.has_argument(a)
    kernel_c_args += a
  endif
endforeach

common_inc = include_directories('core', 'win', '../include', '../include/drm-uapi',
                                 '../include/compat')

# ---------------------------------------------------------------------- import libraries
def_files = {
  'ntoskrnl': toolchain / 'lib' / 'ntoskrnl.def',
  'hal': toolchain / 'lib' / 'hal.def',
  'wdfldr': toolchain / 'lib' / 'wdfldr.def',
}
import_libs = []
foreach name, def : def_files
  import_libs += custom_target('lib' + name + '.a',
    input: def,
    output: 'lib' + name + '.a',
    command: [dlltool, '-m', 'arm64', '-d', '@INPUT@', '-l', '@OUTPUT@'])
endforeach

# ------------------------------------------------------------------------------- core
if get_option('host_tests')
  add_languages('c', native: true)
endif
subdir('win')

if get_option('core') == 'real'
  if not fs.is_file('core' / 'meson.build')
    error('-Dcore=real: kmd/core/meson.build does not exist yet (the core must define pw_core_sources and pw_core_inc); use -Dcore=stub')
  endif
  subdir('core')
  core_sources = pw_core_sources
  core_inc = [pw_core_inc]
else
  core_sources = pw_win_stub_core_sources
  core_inc = []
endif

# ------------------------------------------------------------------------------- link
panwin_objs = static_library('panwin_objs',
  pw_win_sources, core_sources, files('../toolchain/kmdf/fxstub.c'),
  include_directories: [common_inc] + core_inc,
  c_args: kernel_c_args,
  install: false)

kernel_ld_args = [
  '/nologo', '/machine:arm64',
  '/driver', '/driver:wdm',        # both: /driver:wdm alone still warns about /align
  '/subsystem:native,10.0',
  '/entry:FxDriverEntry',
  '/nodefaultlib',
  '/dynamicbase', '/nxcompat', '/highentropyva',
  '/tsaware:no',                   # lld otherwise sets TERMINAL_SERVER_AWARE
  '/release',                      # fill in the PE checksum (the kernel loader checks it)
  '/base:0x140000000', '/filealign:0x1000', '/align:0x1000',
  '/opt:ref', '/opt:icf',
]
# Always write a PDB (WinDbg needs at least the public symbols); full CodeView with -Ddebug=true.
kernel_ld_args += ['/debug:full', '/pdbaltpath:%_PDB%']

panwin_sys = custom_target('panwin.sys',
  input: [panwin_objs] + import_libs,
  output: ['panwin.sys', 'panwin.pdb', 'panwin.map'],
  command: [lld_link, kernel_ld_args,
            '/out:@OUTPUT0@', '/pdb:@OUTPUT1@', '/map:@OUTPUT2@', '@INPUT@'],
  build_by_default: true,
  install: true,
  install_dir: get_option('prefix'))

# --------------------------------------------------------------------------- checks
test('panwin.sys image + imports', python,
  args: ['-I', petool, 'check-sys', panwin_sys[0], '-v',
         '--def', def_files['ntoskrnl'], '--def', def_files['hal'], '--def', def_files['wdfldr'],
         '--approx-ref', toolchain / 'lib' / 'ref' / 'arm32-ntoskrnl.def',
         '--approx-ref', toolchain / 'lib' / 'ref' / 'arm32-hal.def'],
  suite: 'image')

summary({
  'core': get_option('core'),
  'debug ioctls': get_option('debug_ioctls'),
  'KMDF': '1.15 (toolchain/kmdf/fxstub.c)',
  'import defs': 'APPROXIMATE (toolchain/lib, regenerate from real ARM64 binaries)',
}, section: 'panwin.sys')
