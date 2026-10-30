#!/bin/sh
# SPDX-License-Identifier: MIT
#
# update-ddk-headers.sh - re-create toolchain/include/ddk/{wdm,ntddk}.h from a (new) llvm-mingw
# install by applying toolchain/patches/ddk-arm64.patch to its mingw-w64 DDK headers.
#
#   toolchain/scripts/update-ddk-headers.sh [/opt/llvm-mingw]
#
# The headers are public domain (ReactOS DDK, shipped by mingw-w64). If the patch no longer
# applies, port the hunks by hand: they only add an "#elif defined(__aarch64__)" arch block to
# wdm.h and ntddk.h and ARM64 READ/WRITE_REGISTER_* inlines to wdm.h.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
LLVM=${1:-/opt/llvm-mingw}
SRC="$LLVM/aarch64-w64-mingw32/include/ddk"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp "$SRC/wdm.h" "$SRC/ntddk.h" "$TMP/"
(cd "$TMP" && patch -p1 --no-backup-if-mismatch < "$HERE/patches/ddk-arm64.patch")
cp "$TMP/wdm.h" "$TMP/ntddk.h" "$HERE/include/ddk/"
sha256sum "$SRC/wdm.h" "$SRC/ntddk.h"
echo "updated $HERE/include/ddk from $SRC; rebuild and run 'meson test' in the kmd build dir"
