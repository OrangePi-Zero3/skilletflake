#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Build the temporary libdrm.dll stand-in (see libdrm_stub.c) and install it, laid out like a
# libdrm-win install (xf86drm.h at the top, kernel headers in libdrm/, libdrm.pc).
#   build-stub.sh <aarch64|x86_64> <prefix>
set -euo pipefail

arch=$1
prefix=$2
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
llvm_mingw=${LLVM_MINGW:-/opt/llvm-mingw}
cc="$llvm_mingw/bin/$arch-w64-mingw32-clang"
drmwin_inc="$repo/userspace/libdrm-win/include"

mkdir -p "$prefix"/{bin,lib/pkgconfig,include/libdrm/sys}
cp "$drmwin_inc/xf86drm.h" "$drmwin_inc/libdrm_win.h" "$prefix/include/"
cp "$repo"/include/drm-uapi/{drm.h,drm_mode.h,panthor_drm.h} "$prefix/include/libdrm/"
cp "$repo/include/compat/sys/ioccom.h" "$prefix/include/libdrm/sys/"

"$cc" -O2 -shared -mno-ms-bitfields -Wall -Wno-unused-parameter \
	-I"$prefix/include" -I"$prefix/include/libdrm" \
	-o "$prefix/bin/libdrm.dll" -Wl,--out-implib,"$prefix/lib/libdrm.dll.a" \
	"$here/libdrm_stub.c"

cat > "$prefix/lib/pkgconfig/libdrm.pc" <<EOF
prefix=$prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libdrm
Description: STUB libdrm-win stand-in (no devices) from panfrost-win userspace/mesa/stub-libdrm
Version: 2.4.134
Libs: -L\${libdir} -ldrm
Cflags: -I\${includedir} -I\${includedir}/libdrm
EOF
echo "stub libdrm installed in $prefix"
