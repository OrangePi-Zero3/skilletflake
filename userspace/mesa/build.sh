#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Cross-build Mesa's panvk (vulkan_panfrost.dll + ICD json) for Windows with llvm-mingw.
#
#   userspace/mesa/build.sh [aarch64|x86_64|all|checkout|host-tools]      (default: aarch64)
#
#   checkout    only step 1;  host-tools  steps 1-2;  all  aarch64 then x86_64
#
# Steps (each one is skipped when already done, so the script is idempotent):
#   1. Mesa checkout: $MESA_SRC on branch 'panwin' = MESA_COMMIT + patches/*.patch (git am).
#   2. Host tools: SPIRV-LLVM-Translator matching the system LLVM, then mesa_clc, vtn_bindgen2
#      and panfrost_compile from the same Mesa tree, installed in $HOST_TOOLS/prefix.
#   3. libdrm: libdrm-win's install ($LIBDRM_WIN_PREFIX, default repo/out/<arm64|x86_64>);
#      falls back to a no-device stub libdrm.dll (stub-libdrm/) with a loud warning.
#   4. meson setup + ninja + meson install (stripped) into $BUILD_ROOT-<arch>/install.
#   5. Checks: imports/exports of the DLL.
#
# Environment overrides: MESA_SRC, MESA_REF_CLONE, HOST_TOOLS, BUILD_ROOT, LIBDRM_WIN_PREFIX,
# LLVM_MINGW, JOBS, MESA_BUILDTYPE, PANWIN_MESA_NO_RESET=1 (never touch the Mesa checkout).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
refs=$(cd "$repo/../references" && pwd)

MESA_COMMIT=$(tr -d '[:space:]' < "$here/MESA_COMMIT")
MESA_SRC=${MESA_SRC:-$refs/mesa-src}
MESA_REF_CLONE=${MESA_REF_CLONE:-$refs/mesa}
MESA_URL=${MESA_URL:-https://gitlab.freedesktop.org/mesa/mesa.git}
HOST_TOOLS=${HOST_TOOLS:-$refs/host-tools}
BUILD_ROOT=${BUILD_ROOT:-$refs/mesa-build}
LLVM_MINGW=${LLVM_MINGW:-/opt/llvm-mingw}
JOBS=${JOBS:-$(nproc)}
MESA_BUILDTYPE=${MESA_BUILDTYPE:-release}

log() { printf '\n=== %s\n' "$*"; }
die() { printf 'build.sh: error: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- 1. Mesa checkout
series_subjects() {
	local p
	for p in "$here"/patches/*.patch; do
		[ -e "$p" ] || continue
		# Subject header, unfolded (format-patch wraps long subjects).
		awk '/^Subject: /{s=$0; f=1; next} f && /^[ \t]/{s=s $0; next} f{exit} END{
			sub(/^Subject: (\[PATCH[^]]*\] )?/, "", s); gsub(/[ \t]+/, " ", s); print s}' "$p"
	done
}

checkout_mesa() {
	log "Mesa source: $MESA_SRC @ $MESA_COMMIT + $(ls "$here"/patches/*.patch 2>/dev/null | wc -l) patches"
	if [ ! -e "$MESA_SRC/.git" ]; then
		if [ -d "$MESA_REF_CLONE/.git" ]; then
			git -C "$MESA_REF_CLONE" cat-file -e "$MESA_COMMIT^{commit}" 2>/dev/null ||
				git -C "$MESA_REF_CLONE" fetch --filter=blob:none origin "$MESA_COMMIT"
			git -C "$MESA_REF_CLONE" worktree add --detach "$MESA_SRC" "$MESA_COMMIT"
		else
			git clone --filter=blob:none --no-checkout "$MESA_URL" "$MESA_SRC"
			git -C "$MESA_SRC" checkout --detach "$MESA_COMMIT"
		fi
	fi
	[ "${PANWIN_MESA_NO_RESET:-0}" = 1 ] && { echo "PANWIN_MESA_NO_RESET=1: using the checkout as is"; return; }

	git -C "$MESA_SRC" cat-file -e "$MESA_COMMIT^{commit}" 2>/dev/null ||
		git -C "$MESA_SRC" fetch --filter=blob:none origin "$MESA_COMMIT"

	# Already applied? Compare the commit subjects on top of the pin with the series.
	local have want
	have=$(git -C "$MESA_SRC" log --reverse --format=%s "$MESA_COMMIT..HEAD" 2>/dev/null || true)
	want=$(series_subjects)
	if git -C "$MESA_SRC" merge-base --is-ancestor "$MESA_COMMIT" HEAD 2>/dev/null &&
	   [ "$have" = "$want" ]; then
		echo "series already applied (branch $(git -C "$MESA_SRC" rev-parse --abbrev-ref HEAD))"
		return
	fi
	git -C "$MESA_SRC" diff --quiet && git -C "$MESA_SRC" diff --cached --quiet ||
		die "$MESA_SRC has uncommitted changes; commit/stash them or set PANWIN_MESA_NO_RESET=1"
	# Branch 'panwin', unless another worktree of the same repository has it checked out.
	local br=${PANWIN_BRANCH:-panwin} top
	top=$(cd "$MESA_SRC" && pwd -P)
	if git -C "$MESA_SRC" worktree list --porcelain |
	   awk -v b="branch refs/heads/$br" -v t="worktree $top" '/^worktree /{w=$0} $0==b && w!=t {f=1} END{exit !f}'; then
		echo "branch $br is checked out in another worktree; applying the series detached"
		git -C "$MESA_SRC" checkout -q --detach "$MESA_COMMIT"
	else
		git -C "$MESA_SRC" checkout -q -B "$br" "$MESA_COMMIT"
	fi
	if ls "$here"/patches/*.patch >/dev/null 2>&1; then
		git -C "$MESA_SRC" -c user.name="${GIT_COMMITTER_NAME:-panfrost-win build}" \
			-c user.email="${GIT_COMMITTER_EMAIL:-panfrost-win@localhost}" \
			am -q --committer-date-is-author-date "$here"/patches/*.patch
	fi
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
*) die "usage: $0 [aarch64|x86_64|all|checkout|host-tools]" ;;
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
