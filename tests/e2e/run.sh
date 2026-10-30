#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# End-to-end tests of the userspace stack against the REAL kernel core, hardware-free, all
# x86_64-w64-mingw32 under wine (libdrm-win transport "core", PANWIN_TRANSPORT=core):
#
#   csf: Mesa panvk (vulkan_panfrost.dll) -> libdrm-win (libdrm.dll) -> in-process kmd/core
#        + mock G610 (kmd/tests/pw_os_mock.c, mock_csf.c) booting the real mali_csffw.bin
#   jm:  (jm-e2e) userspace/libdrm-win/tests/test_libdrm_jm.c -> libdrm.dll -> kmd/core JM backend
#        + mock G31 (kmd/tests/mock_jm.c), PANWIN_CORE_MODEL=g31. No Mesa: Mesa's panfrost kmod
#        is not built for Windows yet (docs/status/jm-arch.md open question 1), so the test makes
#        the libdrm calls Mesa's panfrost_kmod.c / panvk JM / Gallium pan_jm.c would make.
#
#   tests/e2e/run.sh                 build what is missing, then run every step
#   tests/e2e/run.sh --rebuild       also force libdrm-win (core) and Mesa to rebuild
#   tests/e2e/run.sh [--rebuild] jm  only the libdrm-win build + the jm step (no Mesa needed)
#   tests/e2e/run.sh [--rebuild] csf only the libdrm-win build + the CSF/Mesa steps
#
# Steps:
#   1. libdrm-win with -Dcore_transport=true -> out/x86_64-core (+ its meson tests: libdrm_core
#      (CS-level checks on the G610), libdrm_core_g31 and libdrm_jm (G31), libdrm_fake, ioctl_map)
#   jm. out/e2e-jm-x86_64/: the core libdrm.dll + test_libdrm_jm.exe, run under wine
#   csf 2. Mesa x86_64 (userspace/mesa/build.sh, debugoptimized = asserts on) linked against it,
#      in $E2E_MESA_BUILD_ROOT-x86_64 (default references/mesa-build-e2e-x86_64)
#   csf 3. out/e2e-x86_64/: vulkan_panfrost.dll + the core libdrm.dll + vk_e2e.exe + icd_smoke.exe
#   csf 4. wine: vk_e2e.exe (tests/e2e/vk_e2e.c) and Mesa's icd_smoke (--create-device)
# Environment: E2E_MESA_BUILD_ROOT, MESA_SRC, LLVM_MINGW, PANWIN_CORE_FW, WINEDEBUG, PANVK_DEBUG,
#   PANWIN_JM_STRESS (jm step: number of stress submits, default 1000).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
refs=$(cd "$repo/../references" && pwd)
LLVM_MINGW=${LLVM_MINGW:-/opt/llvm-mingw}
MESA_SRC=${MESA_SRC:-$refs/mesa-src}
E2E_MESA_BUILD_ROOT=${E2E_MESA_BUILD_ROOT:-$refs/mesa-build-e2e}
rebuild=0
do_csf=0
do_jm=0
for a in "$@"; do
	case "$a" in
	--rebuild) rebuild=1 ;;
	csf) do_csf=1 ;;
	jm) do_jm=1 ;;
	*) echo "usage: $0 [--rebuild] [csf] [jm]" >&2; exit 2 ;;
	esac
done
if [ $do_csf = 0 ] && [ $do_jm = 0 ]; then
	do_csf=1
	do_jm=1
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

# ---- csf: Mesa panvk on the G610
if [ $do_csf = 1 ]; then
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
	crc=0
	log "vk_e2e (Vulkan flow on panvk -> libdrm-win -> kmd/core + mock G610)"
	(cd "$run" && PANWIN_CORE_REPORT=1 wine ./vk_e2e.exe vulkan_panfrost.dll) || crc=1
	log "Mesa icd_smoke --require-device --create-device on the core transport"
	(cd "$run" && PANWIN_CORE_REPORT=1 \
		wine ./icd_smoke.exe vulkan_panfrost.dll --require-device --create-device) || crc=1
	[ $crc = 0 ] || rc=1
	results="$results csf=$([ $crc = 0 ] && echo PASS || echo FAIL)"
fi

log "e2e: $([ $rc = 0 ] && echo PASS || echo FAIL) ($results )"
exit $rc
