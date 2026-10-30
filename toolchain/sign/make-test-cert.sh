#!/bin/sh
# SPDX-License-Identifier: MIT
#
# make-test-cert.sh - create a self-signed code-signing certificate for TEST-SIGNING panwin.sys.
#
#   toolchain/sign/make-test-cert.sh [OUTDIR]        (default OUTDIR: toolchain/sign/out)
#
# Produces in OUTDIR:
#   panwin-test.key  RSA-3072 private key (PEM, unencrypted - keep it out of git)
#   panwin-test.crt  self-signed X.509 certificate (PEM), EKU codeSigning, 10 years
#   panwin-test.cer  same certificate, DER: import on the Windows device into
#                    LocalMachine\Root and LocalMachine\TrustedPublisher
#   panwin-test.pfx  key + certificate (PKCS#12, password from $PFX_PASS, default "panwin"):
#                    for signtool / Set-AuthenticodeSignature on Windows (catalog signing)
#
# Test signing only works with "bcdedit /set testsigning on" (Secure Boot off); see
# docs/bringup/windows-install.md. Never use this certificate for anything else.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
CN=${CN:-panfrost-win test signing}
PFX_PASS=${PFX_PASS:-panwin}

command -v openssl >/dev/null 2>&1 || { echo "error: openssl not found" >&2; exit 1; }
mkdir -p "$OUT"
umask 077

if [ -e "$OUT/panwin-test.key" ]; then
	echo "$OUT/panwin-test.key already exists; delete $OUT to regenerate" >&2
	exit 1
fi

openssl req -x509 -newkey rsa:3072 -sha256 -days 3650 -nodes \
	-keyout "$OUT/panwin-test.key" -out "$OUT/panwin-test.crt" \
	-subj "/CN=$CN" \
	-addext "extendedKeyUsage=codeSigning" \
	-addext "keyUsage=critical,digitalSignature" \
	-addext "basicConstraints=critical,CA:FALSE" \
	-addext "subjectKeyIdentifier=hash"
openssl x509 -in "$OUT/panwin-test.crt" -outform DER -out "$OUT/panwin-test.cer"
# OpenSSL 3 encrypts PKCS#12 with PBES2/AES-256. Recent Windows 10/11 import that; if an import
# fails with "password incorrect", re-export with PFX_LEGACY=1 (3DES/SHA1, -legacy). [unverified]
openssl pkcs12 -export -inkey "$OUT/panwin-test.key" -in "$OUT/panwin-test.crt" \
	-name "$CN" -out "$OUT/panwin-test.pfx" -passout "pass:$PFX_PASS" \
	${PFX_LEGACY:+-legacy}

echo "created in $OUT:"
ls -l "$OUT"
openssl x509 -in "$OUT/panwin-test.crt" -noout -subject -fingerprint -sha1 -ext extendedKeyUsage
