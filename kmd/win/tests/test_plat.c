// SPDX-License-Identifier: MIT
/*
 * Host test: board identification from ACPI hardware IDs (kmd/win/pw_win_plat.c). The point is
 * that the RK3588-only PMU check can only ever be selected by an RK3588 ID.
 */
#include <stdio.h>
#include <string.h>

#include "pw_win_plat.h"

static int failures, checks;

#define CHECK(c)                                                                     \
	do {                                                                         \
		checks++;                                                            \
		if (!(c)) {                                                          \
			failures++;                                                  \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
		}                                                                    \
	} while (0)

/* Build a REG_MULTI_SZ from '|'-separated ASCII; returns its length in UTF-16 units. */
static size_t multi_sz(uint16_t *out, size_t cap, const char *s)
{
	size_t n = 0;

	for (; *s && n + 2 < cap; s++)
		out[n++] = *s == '|' ? 0 : (uint16_t)(unsigned char)*s;
	out[n++] = 0;
	out[n++] = 0;
	return n;
}

static const struct pw_win_platform *lookup(const char *ids)
{
	uint16_t buf[256];
	size_t n = multi_sz(buf, 256, ids);

	return pw_win_platform_from_hwids(buf, n);
}

int main(void)
{
	const struct pw_win_platform *rk, *aw;
	uint16_t buf[64];
	size_t n;

	rk = pw_win_platform_by_hid("RKCP7410");
	aw = pw_win_platform_by_hid("awgp0031");
	CHECK(rk && rk->id == PW_WIN_PLAT_RK3588 && rk->csf && rk->rk3588_pmu && !rk->acpi_psc);
	CHECK(rk && rk->mmio_size == 0x200000u && rk->gsiv[0] == 124 && rk->gsiv[2] == 126);
	CHECK(aw && aw->id == PW_WIN_PLAT_H616 && !aw->csf && !aw->rk3588_pmu && aw->acpi_psc);
	CHECK(aw && aw->mmio_size == 0x40000u && aw->gsiv[0] == 127 && aw->gsiv[1] == 128 &&
	      aw->gsiv[2] == 129);
	CHECK(!pw_win_platform_by_hid("AWGP003"));
	CHECK(!pw_win_platform_by_hid("AWGP00310"));
	CHECK(!pw_win_platform_by_hid(NULL));

	/* The forms the ACPI enumerator reports. */
	CHECK(lookup("ACPI\\AWGP0031") == aw);
	CHECK(lookup("*AWGP0031") == aw);
	CHECK(lookup("acpi\\ven_awgp&dev_0031") == aw);
	CHECK(lookup("ACPI\\VEN_AWGP&DEV_0031&SUBSYS_00000000&REV_0000") == aw);
	CHECK(lookup("ACPI\\VEN_AWGP&DEV_0031|ACPI\\AWGP0031|*AWGP0031") == aw);
	CHECK(lookup("ACPI\\RKCP7410|*RKCP7410") == rk);
	CHECK(lookup("ACPI\\VEN_RKCP&DEV_7410") == rk);
	/* Second string matches. */
	CHECK(lookup("ROOT\\SOMETHING|ACPI\\AWGP0031") == aw);
	/* Near misses: never select a platform (above all, never the RK3588 one). */
	CHECK(lookup("ACPI\\AWGP0032") == NULL);
	CHECK(lookup("ACPI\\AWGP00310") == NULL);
	CHECK(lookup("ACPI\\VEN_AWGP&DEV_00311") == NULL);
	CHECK(lookup("AWGP0031") == NULL);
	CHECK(lookup("ACPI\\RKCP741") == NULL);
	CHECK(lookup("") == NULL);
	CHECK(pw_win_platform_from_hwids(NULL, 10) == NULL);
	/* Not NUL-terminated within @chars: bounded by chars. */
	n = 0;
	for (const char *s = "ACPI\\AWGP0031"; *s; s++)
		buf[n++] = (uint16_t)*s;
	CHECK(pw_win_platform_from_hwids(buf, n) == aw);
	CHECK(pw_win_platform_from_hwids(buf, n - 1) == NULL);
	/* Non-ASCII code units never match. */
	n = multi_sz(buf, 64, "ACPI\\AWGP0031");
	buf[5] = 0x0141;
	CHECK(pw_win_platform_from_hwids(buf, n) == NULL);

	printf("test_plat: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
