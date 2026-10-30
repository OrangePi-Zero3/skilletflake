#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# End-to-end test of the whole userspace stack against the REAL kernel core, hardware-free:
#   Mesa panvk (vulkan_panfrost.dll) -> libdrm-win (libdrm.dll) -> in-process kmd/core
#   + mock G610 (kmd/tests/pw_os_mock.c, mock_csf.c) booting the real mali_csffw.bin,
# all x86_64-w64-mingw32 under wine (libdrm-win transport "core", PANWIN_TRANSPORT=core).
#
#   tests/e2e/run.sh            build what is missing, then run every test
#   tests/e2e/run.sh --rebuild  also force libdrm-win (core) and Mesa to rebuild
#
# Steps:
#   1. libdrm-win with -Dcore_transport=true -> out/x86_64-core (+ its meson tests, which
#      include tests/test_libdrm_core.c: CS-level checks through libdrm against the core)
#   2. Mesa x86_64 (userspace/mesa/build.sh, debugoptimized = asserts on) linked against it,
#      in $E2E_MESA_BUILD_ROOT-x86_64 (default references/mesa-build-e2e-x86_64)
#   3. out/e2e-x86_64/: vulkan_panfrost.dll + the core libdrm.dll + vk_e2e.exe + icd_smoke.exe
#   4. wine: vk_e2e.exe (tests/e2e/vk_e2e.c) and Mesa's icd_smoke (--create-device)
# Environment: E2E_MESA_BUILD_ROOT, MESA_SRC, LLVM_MINGW, PANWIN_CORE_FW, WINEDEBUG, PANVK_DEBUG.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
refs=$(cd "$repo/../references" && pwd)
LLVM_MINGW=${LLVM_MINGW:-/opt/llvm-mingw}
MESA_SRC=${MESA_SRC:-$refs/mesa-src}
E2E_MESA_BUILD_ROOT=${E2E_MESA_BUILD_ROOT:-$refs/mesa-build-e2e}
rebuild=0
[ "${1:-}" = --rebuild ] && rebuild=1

log() { printf '\n=== %s\n' "$*"; }

# ---- 1. libdrm-win + core transport
log "libdrm-win (x86_64, -Dcore_transport=true)"
bdir="$repo/out/build/libdrm-win-x86_64-core"
prefix="$repo/out/x86_64-core"
if [ ! -f "$bdir/build.ninja" ]; then
	meson setup "$bdir" "$repo/userspace/libdrm-win" \
		--cross-file "$repo/userspace/libdrm-win/cross/x86_64-w64-mingw32.ini" \
		--prefix "$prefix" --libdir lib -Dfake_kernel=true -Dcore_transport=true
fi
ninja -C "$bdir"
meson install -C "$bdir" --quiet
meson test -C "$bdir" --print-errorlogs

# ---- 2. Mesa against it
mesa_bin="$E2E_MESA_BUILD_ROOT-x86_64/install/bin"
if [ "$rebuild" = 1 ] || [ ! -f "$mesa_bin/vulkan_panfrost.dll" ]; then
	log "Mesa x86_64 (debugoptimized) against $prefix"
	LIBDRM_WIN_PREFIX="$prefix" BUILD_ROOT="$E2E_MESA_BUILD_ROOT" \
		MESA_BUILDTYPE=${MESA_BUILDTYPE:-debugoptimized} MESA_SRC="$MESA_SRC" \
		"$repo/userspace/mesa/build.sh" x86_64
fi

# ---- 3. run directory
run="$repo/out/e2e-x86_64"
mkdir -p "$run"
cp "$mesa_bin/vulkan_panfrost.dll" "$run/"
cp "$prefix/bin/libdrm.dll" "$run/"      # the core-transport libdrm, whatever Mesa linked
cc="$LLVM_MINGW/bin/x86_64-w64-mingw32-clang"
"$cc" -O1 -g -Wall -Wextra -Werror -I"$MESA_SRC/include" -I"$prefix/include" \
	-o "$run/vk_e2e.exe" "$here/vk_e2e.c"
"$cc" -O1 -Wall -I"$MESA_SRC/include" -o "$run/icd_smoke.exe" \
	"$repo/userspace/mesa/tests/icd_smoke.c"

# ---- 4. run
cd "$run"
export WINEDEBUG=${WINEDEBUG:--all} PANWIN_TRANSPORT=core PANWIN_CORE_REPORT=1
rc=0
log "vk_e2e (Vulkan flow on panvk -> libdrm-win -> kmd/core + mock G610)"
wine ./vk_e2e.exe vulkan_panfrost.dll || rc=1
log "Mesa icd_smoke --require-device --create-device on the core transport"
wine ./icd_smoke.exe vulkan_panfrost.dll --require-device --create-device || rc=1
log "e2e: $([ $rc = 0 ] && echo PASS || echo FAIL)"
exit $rc
