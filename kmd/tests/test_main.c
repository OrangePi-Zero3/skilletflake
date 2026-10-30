// SPDX-License-Identifier: MIT
/* test_main.c - runs all kmd/core host test suites. Usage: pw_tests [name-substring] */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mock_gpu.h"
#include "test.h"

#ifndef PW_FW_PATH_DEFAULT
#define PW_FW_PATH_DEFAULT "../../../references/firmware/mali_csffw.bin"
#endif

int g_test_failures;
const char *g_fw_path;
static int g_case_failures;
static int g_skips;

void test_fail(const char *file, int line, const char *expr)
{
	fprintf(stderr, "    FAIL %s:%d: %s\n", file, line, expr);
	g_case_failures++;
}

void test_fail_eq(const char *file, int line, const char *a, const char *b,
		  unsigned long long va, unsigned long long vb)
{
	fprintf(stderr, "    FAIL %s:%d: %s == %s (0x%llx != 0x%llx)\n", file, line, a, b, va, vb);
	g_case_failures++;
}

void test_skip(const char *why)
{
	fprintf(stderr, "    SKIP: %s\n", why);
	g_skips++;
}

uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

bool wait_until(bool (*cond)(void *), void *arg, unsigned int timeout_ms)
{
	uint64_t end = now_ms() + timeout_ms;

	while (!cond(arg)) {
		if (now_ms() > end)
			return cond(arg);
		usleep(1000);
	}
	return true;
}

void *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	void *buf;
	long len;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(len > 0 ? (size_t)len : 1);
	if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*size = (size_t)len;
	return buf;
}

void check_no_leaks(struct pw_os *os)
{
	CHECK_EQ(mock_live_allocs(os), 0);
	CHECK_EQ(mock_live_mems(os), 0);
	CHECK_EQ(mock_live_sync(os), 0);
}

static const struct test_case *const suites[] = {
	util_tests, fw_parse_tests, mmu_tests, device_tests, mem_tests, sync_tests, sched_tests,
	jm_hw_tests, jm_mem_tests, jm_sched_tests,
};

int main(int argc, char **argv)
{
	const char *filter = argc > 1 ? argv[1] : NULL;
	int ran = 0, failed = 0;
	FILE *f;

	g_fw_path = getenv("PW_FW_PATH");
	if (!g_fw_path)
		g_fw_path = PW_FW_PATH_DEFAULT;
	f = fopen(g_fw_path, "rb");
	if (f) {
		fclose(f);
	} else {
		fprintf(stderr, "note: firmware '%s' not found (set PW_FW_PATH); firmware tests skip\n",
			g_fw_path);
		g_fw_path = NULL;
	}

	for (size_t s = 0; s < sizeof(suites) / sizeof(suites[0]); s++) {
		for (const struct test_case *t = suites[s]; t->name; t++) {
			uint64_t t0;

			if (filter && !strstr(t->name, filter))
				continue;
			g_case_failures = 0;
			fprintf(stderr, "[ RUN  ] %s\n", t->name);
			t0 = now_ms();
			t->fn();
			ran++;
			if (g_case_failures) {
				failed++;
				fprintf(stderr, "[ FAIL ] %s (%d checks, %llu ms)\n", t->name,
					g_case_failures, (unsigned long long)(now_ms() - t0));
			} else {
				fprintf(stderr, "[  OK  ] %s (%llu ms)\n", t->name,
					(unsigned long long)(now_ms() - t0));
			}
		}
	}
	fprintf(stderr, "\n%d tests, %d failed, %d skips\n", ran, failed, g_skips);
	return failed ? 1 : 0;
}
