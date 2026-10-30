// SPDX-License-Identifier: MIT
/*
 * pw_win_plat.c - board/SoC identification from the ACPI hardware IDs (pw_win_plat.h).
 *
 * The resources are what our firmware patches put in _CRS:
 *   RK3588: firmware/edk2-rk3588/patches/0001 (0xFB000000 + 0x200000, GSIV 124/125/126)
 *   H616:   firmware/zero3/patches/0001       (0x01800000 + 0x40000, GSIV 127/128/129: GIC SPI
 *           95/96/97 of the Linux sun50i-h616.dtsi gpu@1800000 node)
 * They are only compared against (and logged); the driver uses what PnP hands it.
 */
#include "pw_win_plat.h"

static const struct pw_win_platform pw_win_platforms[] = {
	{
		.id = PW_WIN_PLAT_RK3588,
		.hid = "RKCP7410",
		.soc = "RK3588",
		.gpu = "Mali-G610",
		.mmio_base = 0xFB000000u,
		.mmio_size = 0x200000u,
		.gsiv = { 124, 125, 126 },
		.csf = true,
		.rk3588_pmu = true,
		.acpi_psc = false,	/* the edk2 node has no _PSC; the PMU check covers it */
	},
	{
		.id = PW_WIN_PLAT_H616,
		.hid = "AWGP0031",
		.soc = "H616/H618",
		.gpu = "Mali-G31 MP2",
		.mmio_base = 0x01800000u,
		.mmio_size = 0x40000u,
		.gsiv = { 127, 128, 129 },
		.csf = false,
		.rk3588_pmu = false,	/* 0xFD8D8000 is not a PMU on this SoC: never touch it */
		.acpi_psc = true,	/* _PSC 0 = PPU on, bus gate on, reset released */
	},
};

static int pw_win_lower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* Case-insensitive match of the ASCII @lit against @s[0..n). */
static bool pw_win_match(const uint16_t *s, size_t n, const char *lit)
{
	size_t i;

	for (i = 0; lit[i]; i++)
		if (i >= n || s[i] > 0x7f || pw_win_lower(s[i]) != pw_win_lower((unsigned char)lit[i]))
			return false;
	return i == n;
}

/* Case-insensitive prefix match; returns the length of @lit, or 0. */
static size_t pw_win_prefix(const uint16_t *s, size_t n, const char *lit)
{
	size_t i;

	for (i = 0; lit[i]; i++)
		if (i >= n || s[i] > 0x7f || pw_win_lower(s[i]) != pw_win_lower((unsigned char)lit[i]))
			return 0;
	return i;
}

const struct pw_win_platform *pw_win_platform_by_hid(const char *hid)
{
	if (!hid)
		return NULL;
	for (size_t p = 0; p < sizeof(pw_win_platforms) / sizeof(pw_win_platforms[0]); p++) {
		const char *a = pw_win_platforms[p].hid, *b = hid;

		while (*a && *b && pw_win_lower((unsigned char)*a) == pw_win_lower((unsigned char)*b)) {
			a++;
			b++;
		}
		if (!*a && !*b)
			return &pw_win_platforms[p];
	}
	return NULL;
}

/* One hardware ID string against one platform. */
static bool pw_win_id_matches(const uint16_t *s, size_t n, const struct pw_win_platform *p)
{
	char acpi[16] = "ACPI\\", star[16] = "*", vendev[32] = "ACPI\\VEN_";
	size_t k;

	for (k = 0; k < 8; k++) {
		acpi[5 + k] = p->hid[k];
		star[1 + k] = p->hid[k];
	}
	/* "ACPI\VEN_" VVVV "&DEV_" DDDD */
	for (k = 0; k < 4; k++)
		vendev[9 + k] = p->hid[k];
	vendev[13] = '&';
	vendev[14] = 'D';
	vendev[15] = 'E';
	vendev[16] = 'V';
	vendev[17] = '_';
	for (k = 0; k < 4; k++)
		vendev[18 + k] = p->hid[4 + k];
	vendev[22] = 0;

	if (pw_win_match(s, n, acpi) || pw_win_match(s, n, star))
		return true;
	k = pw_win_prefix(s, n, vendev);
	return k && (k == n || s[k] == '&');
}

const struct pw_win_platform *pw_win_platform_from_hwids(const uint16_t *ids, size_t chars)
{
	size_t pos = 0;

	if (!ids)
		return NULL;
	while (pos < chars && ids[pos]) {
		size_t len = 0;

		while (pos + len < chars && ids[pos + len])
			len++;
		for (size_t p = 0; p < sizeof(pw_win_platforms) / sizeof(pw_win_platforms[0]); p++)
			if (pw_win_id_matches(ids + pos, len, &pw_win_platforms[p]))
				return &pw_win_platforms[p];
		pos += len + 1;
	}
	return NULL;
}
