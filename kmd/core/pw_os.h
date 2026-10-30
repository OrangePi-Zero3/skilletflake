/* SPDX-License-Identifier: MIT */
/*
 * pw_os.h - the only interface between the OS-neutral Mali core (kmd/core) and its host OS.
 *
 * Implemented by:
 *   kmd/win/pw_os_win.c    - Windows kernel (KMDF)
 *   kmd/tests/pw_os_mock.c - Linux host unit tests (mock register file, malloc'd "physical" pages)
 *
 * Rules for core code: no OS headers, no libc beyond <stdint.h>/<stddef.h>/<stdbool.h>;
 * everything else goes through this file.
 */
#ifndef PW_OS_H
#define PW_OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pw_os; /* opaque, owned by the OS layer */

/* Status codes: 0 = success, negative = error (Linux-style values so ports from panthor read naturally). */
#define PW_OK         0
#define PW_EPERM     -1
#define PW_ENOENT    -2
#define PW_EIO       -5
#define PW_ENOMEM   -12
#define PW_EFAULT   -14
#define PW_EBUSY    -16
#define PW_EINVAL   -22
#define PW_ENOSPC   -28
#define PW_ERANGE   -34
#define PW_ENOSYS   -38
#define PW_ETIMEDOUT -110
#define PW_EOPNOTSUPP -95

/* ---- MMIO (GPU register window, offset relative to the GPU base, e.g. 0xFB000000 on RK3588) ---- */
uint32_t pw_os_reg_read32(struct pw_os *os, uint32_t offset);
void     pw_os_reg_write32(struct pw_os *os, uint32_t offset, uint32_t value);
/* 64-bit registers are LO/HI pairs on Mali; core composes them from 32-bit accesses. */

/* ---- time ---- */
uint64_t pw_os_time_ns(struct pw_os *os);               /* monotonic */
void     pw_os_udelay(struct pw_os *os, uint32_t us);   /* busy wait, any IRQL */
void     pw_os_msleep(struct pw_os *os, uint32_t ms);   /* may sleep, passive level only */

/* ---- logging ---- */
enum pw_log_level { PW_LOG_ERR = 0, PW_LOG_WARN = 1, PW_LOG_INFO = 2, PW_LOG_DBG = 3 };
void pw_os_log(struct pw_os *os, enum pw_log_level lvl, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 3, 4)))
#endif
	;

/* ---- general kernel heap (nonpaged) ---- */
void *pw_os_zalloc(struct pw_os *os, size_t size);
void  pw_os_free(struct pw_os *os, void *ptr);

/*
 * ---- GPU-visible memory ----
 * A pw_os_mem is a set of 4 KiB physical pages (not necessarily contiguous unless
 * PW_MEM_CONTIGUOUS), plus an optional kernel CPU mapping. The GPU is NOT IO-coherent on
 * RK3588, so core must call pw_os_mem_flush() after CPU writes the GPU will read and
 * pw_os_mem_invalidate() before CPU reads of GPU-written data, unless the mapping is uncached/WC.
 */
enum pw_mem_flags {
	PW_MEM_CONTIGUOUS = 1u << 0, /* physically contiguous (page tables may want this) */
	PW_MEM_KMAP       = 1u << 1, /* create a kernel CPU mapping (pw_os_mem_kva) */
	PW_MEM_UNCACHED   = 1u << 2, /* CPU mapping is Normal non-cacheable / write-combined */
};

struct pw_os_mem; /* opaque */

int       pw_os_mem_alloc(struct pw_os *os, size_t size, uint32_t flags, struct pw_os_mem **out);
void      pw_os_mem_free(struct pw_os *os, struct pw_os_mem *mem);
size_t    pw_os_mem_size(const struct pw_os_mem *mem);
void     *pw_os_mem_kva(const struct pw_os_mem *mem);              /* NULL unless PW_MEM_KMAP */
uint64_t  pw_os_mem_page_pa(const struct pw_os_mem *mem, size_t page_index); /* 4 KiB pages */
void      pw_os_mem_flush(struct pw_os *os, struct pw_os_mem *mem, size_t offset, size_t size);
void      pw_os_mem_invalidate(struct pw_os *os, struct pw_os_mem *mem, size_t offset, size_t size);

/* ---- locks (passive-level mutex; core never holds one in an ISR) ---- */
struct pw_os_mutex;
struct pw_os_mutex *pw_os_mutex_create(struct pw_os *os);
void pw_os_mutex_destroy(struct pw_os *os, struct pw_os_mutex *m);
void pw_os_mutex_lock(struct pw_os_mutex *m);
void pw_os_mutex_unlock(struct pw_os_mutex *m);

/* ---- events (for "wait until IRQ bottom half saw X") ---- */
struct pw_os_event;
struct pw_os_event *pw_os_event_create(struct pw_os *os);
void pw_os_event_destroy(struct pw_os *os, struct pw_os_event *e);
void pw_os_event_signal(struct pw_os_event *e);
void pw_os_event_reset(struct pw_os_event *e);
/* Returns PW_OK or PW_ETIMEDOUT. */
int  pw_os_event_wait(struct pw_os_event *e, uint32_t timeout_ms);

/* ---- firmware file (e.g. "arm/mali/arch10.8/mali_csffw.bin") ---- */
int  pw_os_firmware_load(struct pw_os *os, const char *name, const void **data, size_t *size);
void pw_os_firmware_release(struct pw_os *os, const void *data);

#endif /* PW_OS_H */
