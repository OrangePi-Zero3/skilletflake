#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Build + install libdrm-win and tools/panwin-info for Windows ARM64 and x86_64 with llvm-mingw,
# then run the x86_64 tests under wine.
#
#   userspace/libdrm-win/build.sh [arm64|x86_64|all]   (default: all)
#
# Install prefixes (what the Mesa cross build points PKG_CONFIG_LIBDIR at):
#   out/arm64/   libdrm.dll (no fake kernel), import lib, headers, lib/pkgconfig/libdrm.pc, panwin-info.exe
#   out/x86_64/  same for x86_64, built with -Dfake_kernel=true (dev/test under wine)
# Build trees: out/build/{libdrm-win,panwin-info}-<arch>/
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
what=${1:-all}

build_one() {
	arch=$1 triple=$2 fake=$3
	prefix="$repo/out/$arch"
	bdir="$repo/out/build/libdrm-win-$arch"
	tdir="$repo/out/build/panwin-info-$arch"
	cross="$here/cross/$triple.ini"

	if [ ! -f "$bdir/build.ninja" ]; then
		meson setup "$bdir" "$here" --cross-file "$cross" --prefix "$prefix" \
			--libdir lib -Dfake_kernel="$fake"
	else
		meson configure "$bdir" -Dfake_kernel="$fake" --prefix "$prefix"
	fi
	ninja -C "$bdir"
	meson install -C "$bdir" --quiet

	# panwin-info finds libdrm through the freshly installed libdrm.pc only.
	pcx="$repo/out/build/pkgconfig-$arch.ini"
	printf "[properties]\npkg_config_libdir = ['%s']\n" "$prefix/lib/pkgconfig" > "$pcx"
	if [ ! -f "$tdir/build.ninja" ]; then
		meson setup "$tdir" "$repo/tools/panwin-info" --cross-file "$cross" --cross-file "$pcx" \
			--prefix "$prefix"
	fi
	ninja -C "$tdir"
	meson install -C "$tdir" --quiet
}

case $what in
arm64|all) build_one arm64 aarch64-w64-mingw32 false ;;
esac
case $what in
x86_64|all)
	build_one x86_64 x86_64-w64-mingw32 true
	meson test -C "$repo/out/build/libdrm-win-x86_64" --print-errorlogs
	;;
esac
