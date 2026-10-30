// SPDX-License-Identifier: MIT
/*
 * Host test: pw_win_vsnprintf (kmd/win/pw_fmt.c) must match the C library's snprintf for the
 * supported subset, including truncation and return values.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pw_fmt.h"

static int failures, checks;

#define CHECK(bufsz, ...)                                                                    \
	do {                                                                                 \
		char a[256], b[256];                                                         \
		int ra, rb;                                                                  \
		memset(a, 'X', sizeof(a));                                                   \
		memset(b, 'X', sizeof(b));                                                   \
		ra = pw_win_snprintf(a, (bufsz), __VA_ARGS__);                                   \
		rb = snprintf(b, (bufsz), __VA_ARGS__);                                      \
		checks++;                                                                    \
		if (ra != rb || memcmp(a, b, sizeof(a))) {                                   \
			failures++;                                                          \
			fprintf(stderr, "FAIL %s:%d [%s] size %d: got %d \"%.*s\" want %d \"%.*s\"\n", \
				__FILE__, __LINE__, #__VA_ARGS__, (int)(bufsz), ra,          \
				(int)((bufsz) ? strnlen(a, (bufsz)) : 0), a, rb,             \
				(int)((bufsz) ? strnlen(b, (bufsz)) : 0), b);                \
		}                                                                            \
	} while (0)

int main(void)
{
	static const size_t sizes[] = { 256, 1, 0, 5, 16 };

	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		size_t n = sizes[i];

		CHECK(n, "plain text");
		CHECK(n, "%d %i %u", 0, -1, 4000000000u);
		CHECK(n, "%d|%5d|%-5d|%05d|%+d|% d|%.3d|%8.3d|%-8.3d|", 42, 42, 42, 42, 42, 42, 7, -7, 7);
		CHECK(n, "%x %X %#x %#X %#o %o %#.0o %.0x", 0xbeefu, 0xbeefu, 0x1fu, 0x1fu, 8u, 8u, 0u, 0u);
		CHECK(n, "%08x %-8x| %#010x %#8x", 0xabcu, 0xabcu, 0xabcu, 0xabcu);
		CHECK(n, "%hhx %hhd %hx %hd", 0x1ff, 0x80, 0x1ffff, 0x8000);
		CHECK(n, "%ld %lu %lx", -123456789L, 123456789UL, 0xdeadUL);
		CHECK(n, "%lld %llu %llx %016llx", -9223372036854775807LL - 1, 18446744073709551615ULL,
		      0x123456789abcdefULL, 0xfb000000ULL);
		CHECK(n, "%zu %zx %zd", (size_t)282624, (size_t)0x45000, (size_t)12);
		CHECK(n, "%jd %ju %td", (intmax_t)-5, (uintmax_t)5, (ptrdiff_t)-77);
		CHECK(n, "%s|%10s|%-10s|%.3s|%*s|%-*s|%.*s", "abc", "abc", "abc", "abcdef", 6, "ab", 6,
		      "ab", 2, "xyz");
		CHECK(n, "%s", (const char *)NULL);
		CHECK(n, "%c%c%3c%-3c|", 'a', 'b', 'c', 'd');
		CHECK(n, "100%% %5%|");
		CHECK(n, "%p %p", (void *)0x1234, (void *)NULL);
		CHECK(n, "%*d %-*d %.*d", -6, 1, 4, 2, -1, 3);
		CHECK(n, "GPU_ID %08x arch %u.%u r%up%u", 0xa8670000u, 10u, 8u, 0u, 0u);
	}
	/* Unknown conversion is copied through (glibc behaviour differs, so check by hand). */
	{
		char b[32];
		int r = pw_win_snprintf(b, sizeof(b), "a%qb");

		checks++;
		if (r != 4 || strcmp(b, "a%qb")) {
			failures++;
			fprintf(stderr, "FAIL unknown conversion: %d \"%s\"\n", r, b);
		}
		r = pw_win_snprintf(b, sizeof(b), "trail%");
		checks++;
		if (r != 6 || strcmp(b, "trail%")) {
			failures++;
			fprintf(stderr, "FAIL trailing %%: %d \"%s\"\n", r, b);
		}
	}
	printf("pw_fmt: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
