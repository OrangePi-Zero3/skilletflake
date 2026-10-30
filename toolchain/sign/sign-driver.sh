#!/bin/sh
# SPDX-License-Identifier: MIT
#
# sign-driver.sh - embed an Authenticode (SHA-256, page hashes) test signature in panwin.sys.
#
#   toolchain/sign/sign-driver.sh IN.sys [OUT.sys] [CERTDIR]
#     OUT.sys  default: IN.sys signed in place (via a temporary file)
#     CERTDIR  default: toolchain/sign/out (from make-test-cert.sh)
#
# Uses osslsigncode (https://github.com/mtrojnar/osslsigncode), which is NOT installed on the
# panfrost-win build host. Install it first, e.g.:
#   Arch:   from the AUR (osslsigncode), or build it: cmake + openssl + curl + zlib
#   Debian/Ubuntu: apt install osslsigncode      Fedora: dnf install osslsigncode
#
# The embedded signature is what the kernel checks at load time with testsigning on. A PnP
# install with pnputil additionally needs a signed catalog (panwin.cat), which osslsigncode can
# sign but not create: build the catalog on Windows (Inf2Cat or New-FileCatalog), see
# docs/bringup/windows-install.md.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
IN=${1:?usage: sign-driver.sh IN.sys [OUT.sys] [CERTDIR]}
OUT=${2:-$IN}
CERTDIR=${3:-$HERE/out}

if ! command -v osslsigncode >/dev/null 2>&1; then
	cat >&2 <<'EOF'
error: osslsigncode not found.

panwin.sys can be test-signed on Linux with osslsigncode, which is not installed here.
Install it (Debian/Ubuntu/Fedora: package "osslsigncode"; Arch: AUR "osslsigncode" or build
from https://github.com/mtrojnar/osslsigncode), then re-run this script.

Alternative without osslsigncode: copy panwin.sys and toolchain/sign/out/panwin-test.pfx to a
Windows machine and sign there:
  signtool sign /fd SHA256 /f panwin-test.pfx /p panwin panwin.sys
or in PowerShell (cert imported into Cert:\CurrentUser\My from the .pfx):
  Set-AuthenticodeSignature -FilePath panwin.sys -Certificate $cert -HashAlgorithm SHA256
EOF
	exit 2
fi
for f in panwin-test.crt panwin-test.key; do
	[ -f "$CERTDIR/$f" ] || { echo "error: $CERTDIR/$f missing: run make-test-cert.sh" >&2; exit 1; }
done

TMP="$OUT.signing.$$"
trap 'rm -f "$TMP"' EXIT
# -ph: page hashes (required for kernel images under HVCI); -h sha256.
osslsigncode sign \
	-certs "$CERTDIR/panwin-test.crt" -key "$CERTDIR/panwin-test.key" \
	-h sha256 -ph -n "panfrost-win panwin.sys (test)" \
	-in "$IN" -out "$TMP"
mv -f "$TMP" "$OUT"
trap - EXIT
echo "signed: $OUT"
osslsigncode verify -CAfile "$CERTDIR/panwin-test.crt" -in "$OUT" || {
	echo "warning: verification against the self-signed CA failed" >&2
	exit 1
}
