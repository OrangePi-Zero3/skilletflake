#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Compile AcpiTables/Gpu.asl with iasl and execute _PS0/_PS3/_PSC/_DSM under
# ACPICA's acpiexec against emulated (all-zero) PMU/CRU memory, with a stub
# \_SB.SCMI. Checks the AML control flow only, not the hardware behaviour.
#
# usage: run_aml_test.sh [path/to/Gpu.asl]
#   IASL / ACPIEXEC env vars override the tool paths.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
asl=${1:-$here/../../../../references/edk2-rk3588/edk2-rockchip/Silicon/Rockchip/RK3588/AcpiTables/Gpu.asl}
IASL=${IASL:-iasl}
ACPIEXEC=${ACPIEXEC:-acpiexec}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$here/gpu_aml_test.asl" "$work/"
# The edk2 tree is CRLF; iasl's own preprocessor rejects "#endif\r".
tr -d '\r' < "$asl" > "$work/Gpu.asl"
: > "$work/AcpiTables.h"
cd "$work"
"$IASL" gpu_aml_test.asl > iasl.log 2>&1 || { cat iasl.log; exit 1; }
grep -E 'Compilation successful' iasl.log
"$ACPIEXEC" -b "execute MAIN" gpu_aml_test.aml > exec.log 2>&1 || true
grep -E 'ACPI Debug:' exec.log | sed 's/^ACPI Debug: *//'
if grep -q '"FAIL ' exec.log; then echo "AML TEST: FAILED"; exit 1; fi
grep -A1 '"FAILURES:"' exec.log | grep -q '0x0000000000000000' || { echo "AML TEST: did not complete"; exit 1; }
echo "AML TEST: PASSED"
