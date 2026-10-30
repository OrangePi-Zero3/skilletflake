#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Apply patches/0001 to Silicium-ACPI's H616 DSDT_minimal.dsl, compile it, and run GPU0 _PS0/_PS3/_PSC under ACPICA's acpiexec against
# emulated memory. Checks the register sequence the AML writes, not the hardware's response.
#   usage: run_aml_test.sh [path/to/Silicium-ACPI]   (default: ../references/silicium-acpi)
#   IASL / ACPIEXEC env vars override the tool paths.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
IASL=${IASL:-iasl}
ACPIEXEC=${ACPIEXEC:-acpiexec}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
src=${1:-$here/../../../../references/silicium-acpi}
cp "$src/SoCs/Allwinner/H616/DSDT_minimal.dsl" "$here/gpu_aml_test.asl" "$work/"
cd "$work"
patch -s DSDT_minimal.dsl < "$here/../patches/0001-H616-add-Mali-G31-GPU0-node.diff"
"$IASL" -p dsdt DSDT_minimal.dsl > iasl.log 2>&1 || { cat iasl.log; exit 1; }
"$IASL" -p gpu_aml_test gpu_aml_test.asl >> iasl.log 2>&1 || { cat iasl.log; exit 1; }
grep -c 'Compilation successful' iasl.log
"$ACPIEXEC" -b "execute MAIN" dsdt.aml gpu_aml_test.aml > exec.log 2>&1 || true
grep -E 'ACPI Debug:' exec.log | sed 's/^ACPI Debug: *//'
if grep -q '"FAIL ' exec.log; then echo "AML TEST: FAILED"; exit 1; fi
grep -A1 '"FAILURES:"' exec.log | grep -q '0x0000000000000000' || { cat exec.log | tail -20; echo "AML TEST: did not complete"; exit 1; }
echo "AML TEST: PASSED"
