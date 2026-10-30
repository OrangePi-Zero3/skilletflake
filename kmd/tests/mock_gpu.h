/* SPDX-License-Identifier: MIT */
/*
 * mock_gpu.h - host test double for pw_os.h: a mock MMIO register file with a small behavioural
 * model of an RK3588 Mali-G610 (CSF), fake physical memory with a separate "DRAM" and "CPU
 * cache" view for cached mappings (so missing pw_os_mem_flush() calls are visible to the
 * emulated GPU), an independent AArch64 page-table walker, and an emulated MCU.
 */
#ifndef MOCK_GPU_H
#define MOCK_GPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pw_os.h"

struct pw_device;

/* G610 values. GPU_ID 0xa867 << 16 (Mesa bin/drm-shim.py:93, "g610": "a867"); the feature
 * registers are "Dumped from a G610" in Mesa src/panfrost/drm-shim/panfrost_noop.c:178-194
 * (MIT). Revision/status nibbles of GPU_ID are not known (left 0). */
#define MOCK_G610_GPU_ID		0xa8670000u
#define MOCK_G610_CSF_ID		0x40a0412u
#define MOCK_G610_L2_FEATURES		0x7120306u
#define MOCK_G610_TILER_FEATURES	0x809u
#define MOCK_G610_MEM_FEATURES		0x301u
#define MOCK_G610_MMU_FEATURES		0x2830u		/* 48 VA bits, 40 PA bits */
#define MOCK_G610_THREAD_FEATURES	0x4010000u
#define MOCK_G610_MAX_THREADS		2048u
#define MOCK_G610_MAX_WG_SIZE		1024u
#define MOCK_G610_MAX_BARRIER		1024u
#define MOCK_G610_TEXTURE_FEATURES0	0xc1ffff9eu
#define MOCK_G610_AS_PRESENT		0xffu
#define MOCK_G610_SHADER_PRESENT	0x50005ull
#define MOCK_G610_L2_PRESENT		1ull
#define MOCK_G610_TILER_PRESENT		1ull

/* Emulated FW interface (plausible, not dumped from hardware). */
#define MOCK_FW_IFACE_VERSION		((1u << 24) | (1u << 16) | 0u)	/* v1.1.0 */
#define MOCK_FW_GROUP_NUM		8u
#define MOCK_FW_STREAM_NUM		8u
#define MOCK_FW_CS_FEATURES		(0x5fu | (8u << 8) | (7u << 16))	/* 96 regs, 8 SB */

#define MOCK_PA_BASE			0x100000000ull	/* fake DRAM above 4 GiB */

enum mock_fw_variant {
	MOCK_FW_OK = 0,
	MOCK_FW_NEVER_BOOTS,		/* MCU_CONTROL=AUTO does nothing */
	MOCK_FW_VERSION_ZERO,
	MOCK_FW_BAD_GROUP_NUM,		/* group_num = 2 */
	MOCK_FW_BAD_INPUT_VA,		/* glb input_va outside the shared section */
	MOCK_FW_BAD_STREAM_NUM,		/* stream_num = 40 */
	MOCK_FW_MISMATCHED_CSG,		/* CSG 3 differs from CSG 0 */
	MOCK_FW_IGNORE_DOORBELL,	/* never acks requests */
};

struct mock_config {
	uint32_t gpu_id;		/* 0 = G610 default */
	bool gpu_id_zero;		/* GPU_ID reads 0 */
	bool no_reset_complete;		/* soft reset never raises RESET_COMPLETED */
	bool no_l2_ready;		/* L2_READY never sets */
	enum mock_fw_variant fw_variant;
	const char *fw_path;		/* file served for any firmware name; NULL = ENOENT */
	const void *fw_data;		/* alternative: in-memory image (not copied) */
	size_t fw_size;
	uint32_t as_busy_reads;		/* AS_STATUS reads ACTIVE this many times per command */
	uint64_t pa_base;		/* 0 = MOCK_PA_BASE */
	bool mcu_lenient;		/* fuzzing: boot even if the AS0 mappings look wrong */
	/* Reset/flush/MCU-boot completions appear in RAWSTAT this long after the command
	 * (0 = default 100 us), like real hardware; instant_completion makes them synchronous. */
	uint32_t completion_delay_us;
	bool instant_completion;
	/* GPU RAWSTAT reads from non-IRQ threads don't show RESET_COMPLETED/CLEAN_CACHES_COMPLETED,
	 * forcing completion through the GPU bottom half. */
	bool hide_completions_from_waiters;
};

struct mock_as_state {
	bool enabled;			/* last UPDATE latched a non-UNMAPPED config */
	uint64_t transtab, memattr, transcfg;
	bool locked;
	uint64_t lockaddr;
	uint32_t updates, locks, unlocks;
};

struct mock_glb_snapshot {
	bool valid;
	uint32_t req, ack_irq_mask, poweroff_timer, progress_timer, idle_timer;
	uint64_t core_en_mask;
	uint32_t doorbells;
};

struct pw_os *mock_create(const struct mock_config *cfg);
void mock_destroy(struct pw_os *os);

/* Interrupt delivery: when a device is attached, three threads (one per line, like panthor's
 * threaded IRQs) call pw_irq_top() whenever RAWSTAT & MASK != 0, then pw_irq_bottom(). */
void mock_attach_irq(struct pw_os *os, struct pw_device *dev);
void mock_detach_irq(struct pw_os *os);
/* Number of times a line was asserted but pw_irq_top() did not claim it (should be 0). */
uint32_t mock_irq_unclaimed(struct pw_os *os);
uint32_t mock_irq_delivered(struct pw_os *os, unsigned int line);

/* Register file access from tests (bypassing the model's side effects for reads). */
uint32_t mock_reg(struct pw_os *os, uint32_t offset);
uint32_t mock_rawstat(struct pw_os *os, unsigned int line);	/* 0 job, 1 mmu, 2 gpu */
uint32_t mock_int_mask(struct pw_os *os, unsigned int line);	/* live INT_MASK value */
void mock_get_as(struct pw_os *os, unsigned int as, struct mock_as_state *st);
uint32_t mock_flush_count(struct pw_os *os);
uint32_t mock_last_flush_cmd(struct pw_os *os);
uint32_t mock_soft_resets(struct pw_os *os);
uint32_t mock_model_errors(struct pw_os *os);	/* protocol violations seen by the model */
uint32_t mock_mcu_status(struct pw_os *os);
void mock_glb_snapshot(struct pw_os *os, struct mock_glb_snapshot *s);

/* Fault / event injection. */
void mock_inject_mmu_fault(struct pw_os *os, unsigned int as, uint32_t fault_status, uint64_t va);
void mock_inject_gpu_fault(struct pw_os *os, uint32_t fault_status, uint64_t addr);
void mock_raise_mmu_irq_bits(struct pw_os *os, uint32_t bits);
void mock_mcu_raise_glb_event(struct pw_os *os, uint32_t bits);	/* toggles ack bits */
uint32_t mock_mcu_glb_req(struct pw_os *os);
uint32_t mock_mcu_glb_ack(struct pw_os *os);

/* Emulated GPU MMU walk using the AS configuration latched by the last UPDATE, reading the
 * tables from "DRAM" (what the GPU sees, i.e. only flushed CPU writes).
 * access: 'r', 'w' or 'x'. Returns 0 or a Mali exception code (0xc0+lvl translation,
 * 0xc8+lvl permission, 0xd8+lvl access flag, 0x80 bus fault). */
int mock_gpu_translate(struct pw_os *os, unsigned int as, uint64_t va, char access, uint64_t *pa,
		       int *level, uint64_t *pte);
/* Read GPU-visible bytes at a VA through AS @as. Returns 0 or the fault code. */
int mock_gpu_read(struct pw_os *os, unsigned int as, uint64_t va, void *buf, size_t len);
/* Host pointer to the "DRAM" byte at @pa (NULL if not allocated). */
uint8_t *mock_pa_to_host(struct pw_os *os, uint64_t pa);

/* Logging captured from pw_os_log (all levels). */
bool mock_log_contains(struct pw_os *os, const char *needle);
void mock_log_clear(struct pw_os *os);
const char *mock_last_fw_name(struct pw_os *os);

/* Resource accounting (leak checks on top of LSan). */
long mock_live_allocs(struct pw_os *os);
long mock_live_mems(struct pw_os *os);
long mock_live_sync(struct pw_os *os);	/* mutexes + events */

#endif /* MOCK_GPU_H */
