#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Cross-build Mesa's panvk (vulkan_panfrost.dll + ICD json) for Windows with llvm-mingw.
#
#   userspace/mesa/build.sh [aarch64|x86_64|all|checkout|host-tools|diff]      (default: aarch64)
#
#   checkout    only step 1;  host-tools  steps 1-2;  all  aarch64 then x86_64
#   diff        regenerate patches/panwin-mesa.diff from the fork's git branch ($MESA_DEV_SRC)
#
# The Mesa used here is a hard fork of upstream at MESA_COMMIT: the source tarball of that commit
# (sha256 in MESA_TARBALL_SHA256) with one diff applied (patches/panwin-mesa.diff). The fork is
# developed as branch 'panwin' of a git checkout ($MESA_DEV_SRC); `build.sh diff` exports it.
#
# Steps (each one is skipped when already done, so the script is idempotent):
#   1. Mesa source: $MESA_SRC = the tarball (downloaded once into $MESA_TARBALL_DIR, checked
#      against MESA_TARBALL_SHA256) + patches/panwin-mesa.diff. A changed diff is swapped in
#      place (old one reversed, new one applied) so unchanged files keep their timestamps.
#   2. Host tools: SPIRV-LLVM-Translator matching the system LLVM, then mesa_clc, vtn_bindgen2
#      and panfrost_compile from the same Mesa tree, installed in $HOST_TOOLS/prefix.
#   3. libdrm: libdrm-win's install ($LIBDRM_WIN_PREFIX, default repo/out/<arm64|x86_64>);
#      falls back to a no-device stub libdrm.dll (stub-libdrm/) with a loud warning.
#   4. meson setup + ninja + meson install (stripped) into $BUILD_ROOT-<arch>/install.
#   5. Checks: imports/exports of the DLL.
#
# Environment overrides: MESA_SRC, MESA_TARBALL_DIR, MESA_DEV_SRC, HOST_TOOLS, BUILD_ROOT,
# LIBDRM_WIN_PREFIX, LLVM_MINGW, JOBS, MESA_BUILDTYPE, PANWIN_MESA_NO_RESET=1 (build $MESA_SRC as
# it is, e.g. MESA_SRC=$MESA_DEV_SRC while developing the fork).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
refs=$(cd "$repo/../references" && pwd)

MESA_COMMIT=$(tr -d '[:space:]' < "$here/MESA_COMMIT")
MESA_TARBALL_SHA256=$(tr -d '[:space:]' < "$here/MESA_TARBALL_SHA256")
MESA_TARBALL_URL=${MESA_TARBALL_URL:-https://gitlab.freedesktop.org/mesa/mesa/-/archive/$MESA_COMMIT/mesa-$MESA_COMMIT.tar.gz}
MESA_DIFF=$here/patches/panwin-mesa.diff
MESA_SRC=${MESA_SRC:-$refs/mesa-panwin}
MESA_TARBALL_DIR=${MESA_TARBALL_DIR:-$refs/mesa-tarball}
MESA_DEV_SRC=${MESA_DEV_SRC:-$refs/mesa-src}
HOST_TOOLS=${HOST_TOOLS:-$refs/host-tools}
BUILD_ROOT=${BUILD_ROOT:-$refs/mesa-build}
LLVM_MINGW=${LLVM_MINGW:-/opt/llvm-mingw}
JOBS=${JOBS:-$(nproc)}
MESA_BUILDTYPE=${MESA_BUILDTYPE:-release}

log() { printf '\n=== %s\n' "$*"; }
die() { printf 'build.sh: error: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- 1. Mesa source
fetch_tarball() {
	local tb="$MESA_TARBALL_DIR/mesa-$MESA_COMMIT.tar.gz"
	if [ ! -f "$tb" ]; then
		mkdir -p "$MESA_TARBALL_DIR"
		log "Mesa tarball: $MESA_TARBALL_URL"
		curl -fSL --retry 3 -o "$tb.part" "$MESA_TARBALL_URL"
		mv "$tb.part" "$tb"
	fi
	echo "$MESA_TARBALL_SHA256  $tb" | sha256sum -c --quiet - ||
		die "$tb does not match MESA_TARBALL_SHA256 (delete it to download again)"
	TARBALL=$tb
}

checkout_mesa() {
	[ "${PANWIN_MESA_NO_RESET:-0}" = 1 ] && {
		log "Mesa source: $MESA_SRC as it is (PANWIN_MESA_NO_RESET=1)"
		return
	}
	[ -f "$MESA_DIFF" ] || die "$MESA_DIFF missing"
	local stamp="$MESA_SRC/.panwin-source" applied="$MESA_SRC/.panwin-mesa.diff"
	local want
	want="$MESA_COMMIT $MESA_TARBALL_SHA256 $(sha256sum < "$MESA_DIFF" | cut -d' ' -f1)"
	log "Mesa source: $MESA_SRC = tarball $MESA_COMMIT + $(basename "$MESA_DIFF")"
	if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then
		echo "up to date"
		return
	fi
	if [ -e "$MESA_SRC" ] && [ ! -f "$stamp" ]; then
		die "$MESA_SRC exists but was not made by this script; remove it, or build it as it is with PANWIN_MESA_NO_RESET=1"
	fi

	# Same tarball, new diff: reverse the old diff and apply the new one in place.
	if [ -f "$stamp" ] && [ -f "$applied" ] &&
	   [ "$(cut -d' ' -f1-2 "$stamp")" = "$MESA_COMMIT $MESA_TARBALL_SHA256" ] &&
	   patch -d "$MESA_SRC" -p1 -R -s --dry-run -f < "$applied" >/dev/null &&
	   patch -d "$MESA_SRC" -p1 -R -s -f --no-backup-if-mismatch < "$applied"; then
		if patch -d "$MESA_SRC" -p1 -s -f --dry-run < "$MESA_DIFF" >/dev/null; then
			patch -d "$MESA_SRC" -p1 -s -f --no-backup-if-mismatch < "$MESA_DIFF"
			cp "$MESA_DIFF" "$applied"
			echo "$want" > "$stamp"
			echo "diff updated in place"
			return
		fi
		die "$(basename "$MESA_DIFF") does not apply to the $MESA_COMMIT tarball"
	fi

	# From scratch.
	fetch_tarball
	local tmp="$MESA_SRC.tmp"
	rm -rf "$tmp"
	mkdir -p "$tmp"
	tar -xzf "$TARBALL" -C "$tmp" --strip-components=1
	patch -d "$tmp" -p1 -s -f --no-backup-if-mismatch < "$MESA_DIFF" ||
		die "$(basename "$MESA_DIFF") does not apply to the $MESA_COMMIT tarball"
	cp "$MESA_DIFF" "$tmp/.panwin-mesa.diff"
	echo "$want" > "$tmp/.panwin-source"
	rm -rf "$MESA_SRC"
	mv "$tmp" "$MESA_SRC"
}

# Export branch 'panwin' of the fork's git checkout as the one diff, with the commit log as a
# '#' comment header (patch ignores it).
export_diff() {
	local br=${PANWIN_BRANCH:-panwin}
	[ -e "$MESA_DEV_SRC/.git" ] || die "$MESA_DEV_SRC is not a git checkout of the fork"
	git -C "$MESA_DEV_SRC" merge-base --is-ancestor "$MESA_COMMIT" "$br" ||
		die "branch $br of $MESA_DEV_SRC is not based on $MESA_COMMIT"
	git -C "$MESA_DEV_SRC" diff --quiet && git -C "$MESA_DEV_SRC" diff --cached --quiet ||
		echo "build.sh: note: $MESA_DEV_SRC has uncommitted changes, they are not exported" >&2
	{
		cat <<-EOF
		# panfrost-win's Mesa: a hard fork of Mesa at $MESA_COMMIT.
		#
		# Apply to the source tarball of that commit (userspace/mesa/MESA_TARBALL_SHA256):
		#   tar -xzf mesa-$MESA_COMMIT.tar.gz && cd mesa-$MESA_COMMIT && patch -p1 < panwin-mesa.diff
		# userspace/mesa/build.sh does this. Generated by 'build.sh diff' from branch $br; do not
		# edit by hand. The changes are LLM-written (Generated-by: LLM) and are not for Mesa
		# upstream; see userspace/mesa/AI-POLICY.md.
		#
		# Commits:
		EOF
		git -C "$MESA_DEV_SRC" log --reverse --no-merges --format='%s%n%n%b%n' "$MESA_COMMIT..$br" |
			sed -e 's/^/#   /' -e 's/[[:space:]]*$//'
		git -C "$MESA_DEV_SRC" diff --no-color --no-renames --no-ext-diff "$MESA_COMMIT" "$br"
	} > "$MESA_DIFF.tmp"
	mv "$MESA_DIFF.tmp" "$MESA_DIFF"
	git -C "$MESA_DEV_SRC" diff --shortstat "$MESA_COMMIT" "$br"
	echo "wrote $MESA_DIFF"
}

# ---------------------------------------------------------------- 2. host tools
build_host_tools() {
	local prefix="$HOST_TOOLS/prefix" stamp="$HOST_TOOLS/prefix/.mesa-tools-$MESA_COMMIT"
	if [ -f "$stamp" ] && [ -x "$prefix/bin/mesa_clc" ] && [ -x "$prefix/bin/vtn_bindgen2" ] &&
	   [ -x "$prefix/bin/panfrost_compile" ]; then
		log "host tools: up to date ($prefix/bin)"
		return
	fi
	command -v llvm-config >/dev/null ||
		die "llvm-config not found; install LLVM/clang: sudo pacman -S --needed llvm clang spirv-tools"
	local llvm_ver llvm_mm
	llvm_ver=$(llvm-config --version)
	llvm_mm=$(echo "$llvm_ver" | cut -d. -f1-2)
	export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

	# SPIRV-LLVM-Translator: mesa_clc needs LLVMSPIRVLib >= LLVM major.minor, < major.minor+1.
	if ! pkg-config --exists "LLVMSPIRVLib >= $llvm_mm" 2>/dev/null; then
		log "host tools: SPIRV-LLVM-Translator for LLVM $llvm_ver"
		local src="$HOST_TOOLS/src" tag
		mkdir -p "$src"
		tag=$(git ls-remote --tags --refs https://github.com/KhronosGroup/SPIRV-LLVM-Translator.git \
			"v$llvm_mm.*" | sed 's|.*refs/tags/||' | sort -V | tail -n1)
		[ -n "$tag" ] || die "no SPIRV-LLVM-Translator tag for LLVM $llvm_mm"
		[ -d "$src/SPIRV-LLVM-Translator" ] ||
			git clone -q --depth 1 -b "$tag" https://github.com/KhronosGroup/SPIRV-LLVM-Translator.git \
				"$src/SPIRV-LLVM-Translator"
		local hdr_commit
		hdr_commit=$(tr -d '[:space:]' < "$src/SPIRV-LLVM-Translator/spirv-headers-tag.conf")
		if [ ! -d "$src/SPIRV-Headers" ]; then
			git clone -q https://github.com/KhronosGroup/SPIRV-Headers.git "$src/SPIRV-Headers"
		fi
		git -C "$src/SPIRV-Headers" checkout -q "$hdr_commit"
		cmake -S "$src/SPIRV-LLVM-Translator" -B "$HOST_TOOLS/build/spirv-llvm-translator" -G Ninja \
			-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_SHARED_LIBS=ON \
			-DLLVM_INCLUDE_TESTS=OFF -DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR="$src/SPIRV-Headers"
		ninja -C "$HOST_TOOLS/build/spirv-llvm-translator" -j"$JOBS"
		ninja -C "$HOST_TOOLS/build/spirv-llvm-translator" install
	fi

	log "host tools: mesa_clc, vtn_bindgen2, panfrost_compile (native Mesa build)"
	local hb="$BUILD_ROOT-host"
	if [ ! -f "$hb/build.ninja" ]; then
		meson setup "$hb" "$MESA_SRC" --prefix="$prefix" -Dbuildtype=release \
			-Dvulkan-drivers=panfrost -Dgallium-drivers= -Dplatforms= -Dglx=disabled \
			-Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
			-Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled \
			-Dinstall-precomp-compiler=true -Dpanfrost-rust=false -Dllvm=enabled \
			-Dshared-llvm=enabled -Dbuild-tests=false -Dvalgrind=disabled -Dlibunwind=disabled \
			-Dzstd=disabled -Dvideo-codecs= -Dlmsensors=disabled \
			-Dc_link_args="-Wl,-rpath,$prefix/lib" -Dcpp_link_args="-Wl,-rpath,$prefix/lib"
	fi
	ninja -C "$hb" -j"$JOBS" src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 \
		src/panfrost/clc/panfrost_compile
	mkdir -p "$prefix/bin"
	install -m755 "$hb/src/compiler/clc/mesa_clc" "$hb/src/compiler/spirv/vtn_bindgen2" \
		"$hb/src/panfrost/clc/panfrost_compile" "$prefix/bin/"
	rm -f "$prefix"/bin/.mesa-tools-* "$prefix"/.mesa-tools-*
	touch "$stamp"
}

# ---------------------------------------------------------------- 3. libdrm
LIBDRM_IS_STUB=0
find_libdrm() {
	local arch=$1 bdir=$2 drmarch
	drmarch=$([ "$arch" = aarch64 ] && echo arm64 || echo x86_64)
	local p=${LIBDRM_WIN_PREFIX:-$repo/out/$drmarch}
	if [ -f "$p/lib/pkgconfig/libdrm.pc" ] && [ -f "$p/include/libdrm_win.h" ]; then
		LIBDRM_PREFIX=$p
		log "libdrm: libdrm-win install at $p"
	else
		LIBDRM_PREFIX=$bdir/stub-libdrm
		LIBDRM_IS_STUB=1
		log "libdrm: WARNING libdrm-win not installed at $p (run userspace/libdrm-win/build.sh)."
		echo "         Linking against the no-device STUB libdrm.dll in $LIBDRM_PREFIX instead."
		LLVM_MINGW=$LLVM_MINGW "$here/stub-libdrm/build-stub.sh" "$arch" "$LIBDRM_PREFIX"
	fi
}

# ---------------------------------------------------------------- 4. Mesa cross build
build_mesa() {
	local arch=$1
	local bdir="$BUILD_ROOT-$arch"
	mkdir -p "$bdir"
	find_libdrm "$arch" "$bdir"

	cat > "$bdir/paths.ini" <<-EOF
	[constants]
	llvm_mingw = '$LLVM_MINGW'
	panwin_compat_inc = '$repo/include/compat'
	EOF

	export PATH="$HOST_TOOLS/prefix/bin:$PATH"
	export PKG_CONFIG_LIBDIR="$LIBDRM_PREFIX/lib/pkgconfig"
	export PKG_CONFIG_PATH=
	pkg-config --exists libdrm || die "pkg-config can't find libdrm in $PKG_CONFIG_LIBDIR"

	# Rebuild from scratch when the libdrm prefix changes (stub <-> real).
	if [ -f "$bdir/build/build.ninja" ] && [ "$(cat "$bdir/.libdrm-prefix" 2>/dev/null)" != "$LIBDRM_PREFIX" ]; then
		rm -rf "$bdir/build"
	fi
	echo "$LIBDRM_PREFIX" > "$bdir/.libdrm-prefix"
	# ...and when the Mesa source tree moves (e.g. git checkout -> tarball tree).
	if [ -f "$bdir/build/build.ninja" ] && [ "$(cat "$bdir/.mesa-src" 2>/dev/null)" != "$MESA_SRC" ]; then
		rm -rf "$bdir/build"
	fi
	echo "$MESA_SRC" > "$bdir/.mesa-src"

	log "Mesa: configure $arch ($bdir/build)"
	local opts=(
		-Dprefix="$bdir/install" -Dbuildtype="$MESA_BUILDTYPE"
		-Dvulkan-drivers=panfrost -Dgallium-drivers= -Dplatforms=windows
		-Dopengl=false -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dglx=disabled
		-Dgbm=disabled -Dllvm=disabled -Dmicrosoft-clc=disabled -Dspirv-tools=disabled
		-Dmesa-clc=system -Dprecomp-compiler=system -Dpanfrost-rust=false
		-Dvideo-codecs= -Dzstd=disabled -Dexpat=disabled -Dvalgrind=disabled
		-Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false
		-Dzlib:default_library=static
	)
	if [ -f "$bdir/build/build.ninja" ]; then
		meson configure "$bdir/build" "${opts[@]}" >/dev/null
	else
		meson setup "$bdir/build" "$MESA_SRC" --cross-file "$bdir/paths.ini" \
			--cross-file "$here/cross/$arch-w64-mingw32.ini" "${opts[@]}"
	fi

	log "Mesa: build $arch"
	ninja -C "$bdir/build" -j"$JOBS"
	meson install -C "$bdir/build" --strip --quiet --tags runtime
	# The loader and smoke test need libdrm.dll next to the driver.
	cp "$LIBDRM_PREFIX/bin/libdrm.dll" "$bdir/install/bin/"

	check_dll "$arch" "$bdir"
}

# ---------------------------------------------------------------- 5. checks
check_dll() {
	local arch=$1 bdir=$2
	local dll="$bdir/install/bin/vulkan_panfrost.dll" ro="$LLVM_MINGW/bin/llvm-readobj"
	[ -f "$dll" ] || die "$dll not produced"
	log "Checks: $dll"
	local imports exports bad
	imports=$("$ro" --coff-imports "$dll" | sed -n 's/^ *Name: \(.*\.dll\)$/\1/Ip' | sort -fu)
	exports=$("$ro" --coff-exports "$dll" | sed -n 's/^ *Name: //p' | sort)
	echo "machine: $("$ro" --file-headers "$dll" | sed -n 's/^ *Machine: //p')"
	echo "imports: $(echo $imports)"
	echo "exports: $(echo $exports)"
	bad=$(echo "$imports" | grep -viE '^(libdrm|kernel32|user32|gdi32|api-ms-win-.*)\.dll$' || true)
	[ -z "$bad" ] || die "unexpected DLL imports: $bad"
	for s in vk_icdGetInstanceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion vk_icdGetPhysicalDeviceProcAddr; do
		echo "$exports" | grep -qx "$s" || die "missing export $s"
	done
	ls "$bdir"/install/share/vulkan/icd.d/panfrost_icd.*.json
	[ "$LIBDRM_IS_STUB" = 0 ] || echo "NOTE: linked against the STUB libdrm (no devices)."
}

what=${1:-aarch64}
case $what in
aarch64|x86_64|all|checkout|host-tools) ;;
diff) export_diff; exit 0 ;;
*) die "usage: $0 [aarch64|x86_64|all|checkout|host-tools|diff]" ;;
esac
[ -x "$LLVM_MINGW/bin/aarch64-w64-mingw32-clang" ] || die "llvm-mingw not found at $LLVM_MINGW"

checkout_mesa
[ "$what" = checkout ] && exit 0
build_host_tools
[ "$what" = host-tools ] && exit 0
case $what in
all) build_mesa aarch64; LIBDRM_IS_STUB=0; build_mesa x86_64 ;;
*) build_mesa "$what" ;;
esac
log "done"
