/* SPDX-License-Identifier: MIT */
/*
 * pw_util.h - tiny freestanding replacements for the libc bits the core needs.
 * The core must not call libc (it is linked into a Windows kernel driver); clang may still emit
 * memcpy/memset calls for struct copies, which ntoskrnl exports.
 */
#ifndef PW_UTIL_H
#define PW_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PW_BIT(n)            (1u << (n))
#define PW_BIT64(n)          (1ull << (n))
#define PW_GENMASK(h, l)     (((~0u) >> (31 - (h))) & ~((1u << (l)) - 1u))
#define PW_GENMASK64(h, l)   (((~0ull) >> (63 - (h))) & ~((1ull << (l)) - 1ull))
#define PW_ARRAY_SIZE(a)     (sizeof(a) / sizeof((a)[0]))
#define PW_MIN(a, b)         ((a) < (b) ? (a) : (b))
#define PW_MAX(a, b)         ((a) > (b) ? (a) : (b))
#define PW_IS_ALIGNED(x, a)  (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define PW_ALIGN_UP(x, a)    (((x) + ((__typeof__(x))(a) - 1)) & ~((__typeof__(x))(a) - 1))
#define PW_DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#define PW_SZ_4K   0x1000ull
#define PW_SZ_2M   0x200000ull
#define PW_SZ_1G   0x40000000ull
#define PW_SZ_4G   0x100000000ull

/* Shared-memory accessors (READ_ONCE/WRITE_ONCE equivalents) for FW interface fields. */
#define PW_READ_ONCE(x)      (*(const volatile __typeof__(x) *)&(x))
#define PW_WRITE_ONCE(x, v)  (*(volatile __typeof__(x) *)&(x) = (v))

/*
 * Barriers. The FW interface lives in Normal-NC/WC memory and doorbells are Device memory, so a
 * store barrier is needed between the interface update and the doorbell write (Linux writel()
 * contains one; we don't assume pw_os_reg_write32() does).
 */
#if defined(__aarch64__) || defined(_M_ARM64)
#define pw_wmb() __asm__ __volatile__("dsb st" ::: "memory")
#define pw_rmb() __asm__ __volatile__("dsb ld" ::: "memory")
#define pw_mb()  __asm__ __volatile__("dsb sy" ::: "memory")
#else
#define pw_wmb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define pw_rmb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define pw_mb()  __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

void  *pw_memcpy(void *dst, const void *src, size_t n);
void  *pw_memset(void *dst, int c, size_t n);
int    pw_memcmp(const void *a, const void *b, size_t n);
size_t pw_strlen(const char *s);
size_t pw_strnlen(const char *s, size_t max);
/* Always NUL-terminates when size > 0; returns strlen(src). */
size_t pw_strlcpy(char *dst, const char *src, size_t size);

/*
 * snprintf subset: %d %i %u %x %X %c %s %p %%, with flags '0' '-', width, precision for %s,
 * and length modifiers hh h l ll z. Always NUL-terminates when size > 0.
 * Returns the length that would have been written (like C99 snprintf).
 */
int pw_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int pw_snprintf(char *buf, size_t size, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 3, 4)))
#endif
	;

/* Bit helpers (1-based like Linux fls/ffs; 0 when x == 0). */
static inline unsigned int pw_fls64(uint64_t x)
{
	return x ? 64u - (unsigned int)__builtin_clzll(x) : 0u;
}

static inline unsigned int pw_ffs32(uint32_t x)
{
	return x ? (unsigned int)__builtin_ctz(x) + 1u : 0u;
}

static inline unsigned int pw_hweight64(uint64_t x)
{
	unsigned int n = 0;

	while (x) {
		x &= x - 1;
		n++;
	}
	return n;
}

#endif /* PW_UTIL_H */
