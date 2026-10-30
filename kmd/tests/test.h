/* SPDX-License-Identifier: MIT */
/* test.h - minimal test harness for the kmd/core host tests. */
#ifndef PW_TEST_H
#define PW_TEST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct pw_os;

extern int g_test_failures;
extern const char *g_fw_path;	/* NULL if the real firmware isn't available */

void test_fail(const char *file, int line, const char *expr);
void test_fail_eq(const char *file, int line, const char *a, const char *b,
		  unsigned long long va, unsigned long long vb);
void test_skip(const char *why);

#define CHECK(c) do { if (!(c)) test_fail(__FILE__, __LINE__, #c); } while (0)
#define CHECK_EQ(a, b) do { \
		unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); \
		if (_a != _b) \
			test_fail_eq(__FILE__, __LINE__, #a, #b, _a, _b); \
	} while (0)
#define REQUIRE(c) do { if (!(c)) { test_fail(__FILE__, __LINE__, #c); return; } } while (0)

struct test_case {
	const char *name;
	void (*fn)(void);
};

/* Wait (polling every 1 ms) until cond(arg) is true; false on timeout. */
bool wait_until(bool (*cond)(void *), void *arg, unsigned int timeout_ms);
uint64_t now_ms(void);

/* Read a whole file; caller frees. */
void *read_file(const char *path, size_t *size);

/* Check that every core resource was released (allocations, memory, mutexes/events). */
void check_no_leaks(struct pw_os *os);

extern const struct test_case util_tests[];
extern const struct test_case fw_parse_tests[];
extern const struct test_case mmu_tests[];
extern const struct test_case device_tests[];
extern const struct test_case mem_tests[];
extern const struct test_case sync_tests[];
extern const struct test_case sched_tests[];
extern const struct test_case jm_hw_tests[];
extern const struct test_case jm_mem_tests[];
extern const struct test_case jm_sched_tests[];

#endif /* PW_TEST_H */
