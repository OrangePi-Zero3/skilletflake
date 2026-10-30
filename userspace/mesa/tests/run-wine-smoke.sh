#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Build tests/icd_smoke.c for x86_64 and run it under wine against the x86_64 vulkan_panfrost.dll
# produced by `build.sh x86_64`: once with no device (expect 0 physical devices) and once with
# libdrm-win's fake kernel (PANWIN_FAKE_KERNEL=1, only in libdrm-win builds with -Dfake_kernel).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
refs=$(cd "$here/../../../../references" && pwd)
BUILD_ROOT=${BUILD_ROOT:-$refs/mesa-build}
LLVM_MINGW=${LLVM_MINGW:-/opt/llvm-mingw}
bin="$BUILD_ROOT-x86_64/install/bin"
MESA_SRC=${MESA_SRC:-$refs/mesa-src}

[ -f "$bin/vulkan_panfrost.dll" ] || { echo "run build.sh x86_64 first" >&2; exit 1; }
"$LLVM_MINGW/bin/x86_64-w64-mingw32-clang" -O1 -Wall -I"$MESA_SRC/include" \
	-o "$bin/icd_smoke.exe" "$here/icd_smoke.c"

cd "$bin"
export WINEDEBUG=${WINEDEBUG:--all}
echo "--- no device"
env -u PANWIN_FAKE_KERNEL wine ./icd_smoke.exe vulkan_panfrost.dll
echo "--- libdrm-win fake kernel (PANWIN_FAKE_KERNEL=1, PANVK_DEBUG=startup)"
PANWIN_FAKE_KERNEL=1 PANVK_DEBUG=${PANVK_DEBUG:-startup} \
	wine ./icd_smoke.exe vulkan_panfrost.dll --require-device
echo "--- fake kernel + vkCreateDevice (informational: needs VM_BIND, GROUP_CREATE, ... in the fake)"
PANWIN_FAKE_KERNEL=1 wine ./icd_smoke.exe vulkan_panfrost.dll --require-device --create-device ||
	echo "(vkCreateDevice failed; see the ioctl error above)"
