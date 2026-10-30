#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# End-to-end tests of the userspace stack against the REAL kernel core, hardware-free, all
# x86_64-w64-mingw32 under wine (libdrm-win transport "core", PANWIN_TRANSPORT=core):
#
#   csf: Mesa panvk (vulkan_panfrost.dll) -> libdrm-win (libdrm.dll) -> in-process kmd/core
#        + mock G610 (kmd/tests/pw_os_mock.c, mock_csf.c) booting the real mali_csffw.bin
#   jm:  (jm-e2e) userspace/libdrm-win/tests/test_libdrm_jm.c -> libdrm.dll -> kmd/core JM backend
#        + mock G31 (kmd/tests/mock_jm.c), PANWIN_CORE_MODEL=g31. No Mesa: the libdrm calls
#        Mesa's panfrost_kmod.c / panvk JM / Gallium pan_jm.c make, with errno and fault checks.
#   g31: Mesa panvk v7 -> libdrm.dll -> kmd/core JM backend + mock G31 (PANWIN_CORE_MODEL=g31,
#        PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1: panvk refuses v7 without it).
#
#   tests/e2e/run.sh                  build what is needed, then run every step
#   tests/e2e/run.sh --rebuild        also force libdrm-win (core) and Mesa to rebuild from clean
#   tests/e2e/run.sh [--rebuild] jm   only the libdrm-win build + the jm step (no Mesa needed)
#   tests/e2e/run.sh [--rebuild] csf  only the libdrm-win build + Mesa on the G610
#   tests/e2e/run.sh [--rebuild] g31  only the libdrm-win build + Mesa on the G31
#
# Steps:
#   1. libdrm-win with -Dcore_transport=true -> out/x86_64-core (+ its meson tests: libdrm_core
#      (CS-level checks on the G610), libdrm_core_g31 and libdrm_jm (G31), libdrm_fake, ioctl_map)
#   jm. out/e2e-jm-x86_64/: the core libdrm.dll + test_libdrm_jm.exe, run under wine
#   2. (csf, g31) Mesa x86_64 (userspace/mesa/build.sh: the Mesa tarball + panwin-mesa.diff,
#      debugoptimized = asserts on) linked against it, in $E2E_MESA_BUILD_ROOT-x86_64 (default
#      references/mesa-build-e2e-x86_64). Incremental: build.sh is idempotent.
#   3. out/e2e-x86_64/: vulkan_panfrost.dll + the core libdrm.dll + vk_e2e.exe + icd_smoke.exe
#   csf, g31. wine: vk_e2e.exe (tests/e2e/vk_e2e.c) and Mesa's icd_smoke (--create-device), on
#      the mock G610 or the mock G31
# Environment: E2E_MESA_BUILD_ROOT, MESA_SRC, LLVM_MINGW, PANWIN_CORE_FW, WINEDEBUG, PANVK_DEBUG,
#   PANWIN_JM_STRESS (jm step: number of stress submits, default 1000).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
refs=$(cd "$repo/../references" && pwd)
LLVM_MINGW=${LLVM_MINGW:-/opt/llvm-mingw}
MESA_SRC=${MESA_SRC:-$refs/mesa-panwin}	# what userspace/mesa/build.sh makes
E2E_MESA_BUILD_ROOT=${E2E_MESA_BUILD_ROOT:-$refs/mesa-build-e2e}
rebuild=0
do_csf=0
do_jm=0
do_g31=0
for a in "$@"; do
	case "$a" in
	--rebuild) rebuild=1 ;;
	csf) do_csf=1 ;;
	jm) do_jm=1 ;;
	g31) do_g31=1 ;;
	*) echo "usage: $0 [--rebuild] [csf] [jm] [g31]" >&2; exit 2 ;;
	esac
done
if [ $do_csf = 0 ] && [ $do_jm = 0 ] && [ $do_g31 = 0 ]; then
	do_csf=1
	do_jm=1
	do_g31=1
fi

log() { printf '\n=== %s\n' "$*"; }

# ---- 1. libdrm-win + core transport
log "libdrm-win (x86_64, -Dcore_transport=true)"
bdir="$repo/out/build/libdrm-win-x86_64-core"
prefix="$repo/out/x86_64-core"
if [ ! -f "$bdir/build.ninja" ]; then
	meson setup "$bdir" "$repo/userspace/libdrm-win" \
		--cross-file "$repo/userspace/libdrm-win/cross/x86_64-w64-mingw32.ini" \
		--prefix "$prefix" --libdir lib -Dfake_kernel=true -Dcore_transport=true
elif [ "$rebuild" = 1 ]; then
	ninja -C "$bdir" -t clean >/dev/null
fi
ninja -C "$bdir"
meson install -C "$bdir" --quiet
meson test -C "$bdir" --print-errorlogs

export WINEDEBUG=${WINEDEBUG:--all} PANWIN_TRANSPORT=core
rc=0
results=""

# ---- jm: the G31 job path below Mesa
if [ $do_jm = 1 ]; then
	log "jm (test_libdrm_jm: libdrm-win -> kmd/core JM backend + mock G31)"
	jrun="$repo/out/e2e-jm-x86_64"
	mkdir -p "$jrun"
	cp "$prefix/bin/libdrm.dll" "$jrun/"
	cp "$bdir/test_libdrm_jm.exe" "$jrun/"
	jrc=0
	# PANWIN_CORE_REPORT=1 for the transport's own teardown verdict; the expected ioctl
	# failures it also prints (the test checks errnos on purpose) are filtered out.
	(cd "$jrun" && PANWIN_CORE_MODEL=g31 PANWIN_CORE_REPORT=1 wine ./test_libdrm_jm.exe 2>&1) |
		grep -v '^libdrm-win core: DRM ioctl ' || jrc=1
	[ $jrc = 0 ] || rc=1
	results="$results jm=$([ $jrc = 0 ] && echo PASS || echo FAIL)"
fi

# ---- Mesa panvk (csf, g31)
if [ $do_csf = 1 ] || [ $do_g31 = 1 ]; then
	# ---- 2. Mesa against it
	mesa_bin="$E2E_MESA_BUILD_ROOT-x86_64/install/bin"
	log "Mesa x86_64 (debugoptimized) against $prefix"
	[ "$rebuild" = 1 ] && rm -rf "$E2E_MESA_BUILD_ROOT-x86_64/build"
	LIBDRM_WIN_PREFIX="$prefix" BUILD_ROOT="$E2E_MESA_BUILD_ROOT" \
		MESA_BUILDTYPE=${MESA_BUILDTYPE:-debugoptimized} MESA_SRC="$MESA_SRC" \
		"$repo/userspace/mesa/build.sh" x86_64

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
fi

# ---- csf: Mesa panvk on the G610
if [ $do_csf = 1 ]; then
	crc=0
	log "vk_e2e (Vulkan flow on panvk -> libdrm-win -> kmd/core + mock G610)"
	(cd "$run" && PANWIN_CORE_REPORT=1 wine ./vk_e2e.exe vulkan_panfrost.dll) || crc=1
	log "Mesa icd_smoke --require-device --create-device on the core transport"
	(cd "$run" && PANWIN_CORE_REPORT=1 \
		wine ./icd_smoke.exe vulkan_panfrost.dll --require-device --create-device) || crc=1
	[ $crc = 0 ] || rc=1
	results="$results csf=$([ $crc = 0 ] && echo PASS || echo FAIL)"
fi

# ---- g31: Mesa panvk v7 on the G31
if [ $do_g31 = 1 ]; then
	grc=0
	g31_env=(PANWIN_CORE_MODEL=g31 PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 PANWIN_CORE_REPORT=1)
	log "vk_e2e (Vulkan flow on panvk v7 -> libdrm-win -> kmd/core JM + mock G31)"
	(cd "$run" && env "${g31_env[@]}" PANWIN_E2E_GPU=Mali-G31 \
		wine ./vk_e2e.exe vulkan_panfrost.dll) || grc=1
	log "Mesa icd_smoke --require-device --create-device on the mock G31"
	(cd "$run" && env "${g31_env[@]}" \
		wine ./icd_smoke.exe vulkan_panfrost.dll --require-device --create-device) || grc=1
	[ $grc = 0 ] || rc=1
	results="$results g31=$([ $grc = 0 ] && echo PASS || echo FAIL)"
fi

log "e2e: $([ $rc = 0 ] && echo PASS || echo FAIL) ($results )"
exit $rc
