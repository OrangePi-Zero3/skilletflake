/* SPDX-License-Identifier: MIT */
/*
 * pw_win_plat.h - which board/SoC panwin.sys was started on, from the ACPI hardware IDs of its
 * device node. Everything the glue does that is specific to one SoC (the RK3588 PMU check, the
 * ACPI _PSC check, the expected MMIO window and GSIVs, log text) is keyed on this, so a check
 * written for one SoC can never run on another one (the RK3588 PMU lives at a physical address
 * that is DRAM or nothing at all on the Allwinner H618).
 *
 * No OS headers: built into panwin.sys and into the Linux host tests (kmd/win/tests).
 */
#ifndef PW_WIN_PLAT_H
#define PW_WIN_PLAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum pw_win_plat_id {
	PW_WIN_PLAT_RK3588 = 1,		/* Mali-G610, CSF (edk2-rk3588 patch, _HID RKCP7410) */
	PW_WIN_PLAT_H616 = 2,		/* Mali-G31 MP2, job manager (Zero3 patch, _HID AWGP0031) */
};

struct pw_win_platform {
	enum pw_win_plat_id id;
	const char *hid;		/* ACPI _HID, 8 characters */
	const char *soc;		/* "RK3588", "H616/H618" */
	const char *gpu;		/* "Mali-G610", "Mali-G31 MP2" */
	uint32_t mmio_base;		/* _CRS memory base (diagnostics only) */
	uint32_t mmio_size;		/* _CRS memory length */
	uint32_t gsiv[3];		/* _CRS interrupts in order job, mmu, gpu */
	bool csf;			/* expected backend: true = CSF (panthor), false = JM */
	bool rk3588_pmu;		/* the RK3588 PMU2 PD_GPU check applies (Rk3588PmuCheck) */
	bool acpi_psc;			/* check ACPI _PSC == 0 before touching registers */
};

/* Platform for an ACPI _HID ("RKCP7410"), case-insensitive; NULL if unknown. */
const struct pw_win_platform *pw_win_platform_by_hid(const char *hid);

/*
 * Platform for a REG_MULTI_SZ list of hardware IDs (DevicePropertyHardwareID: UTF-16 strings,
 * each NUL-terminated, the list ended by an empty string or by @chars). Recognised forms of
 * a _HID "VVVVDDDD": "ACPI\VVVVDDDD", "*VVVVDDDD", "ACPI\VEN_VVVV&DEV_DDDD" (optionally followed
 * by "&..."), all case-insensitive. NULL if no ID matches.
 */
const struct pw_win_platform *pw_win_platform_from_hwids(const uint16_t *ids, size_t chars);

#endif /* PW_WIN_PLAT_H */
