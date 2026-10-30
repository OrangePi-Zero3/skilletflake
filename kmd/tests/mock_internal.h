/* SPDX-License-Identifier: MIT */
/*
 * mock_internal.h - interface between the mock's register/memory model (pw_os_mock.c) and its
 * CSF command-stream executor (mock_csf.c) / job-chain executor (mock_jm.c). Not for tests:
 * they use mock_gpu.h.
 */
#ifndef MOCK_INTERNAL_H
#define MOCK_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mock_gpu.h"

/* Emulated interface layout inside the 64 KiB shared section at MCU VA 0x04000000. */
#define SH_VA		0x04000000u
#define SH_SIZE		0x10000u
#define GLB_IN		0x100u
#define GLB_OUT		0x200u
#define CSG_CTRL(i)	(0x1000u + (i) * 0x100u)
#define CS_CTRL(i, j)	(CSG_CTRL(i) + 0x40u + (j) * 0xcu)
#define CSG_IN(i)	(0x3000u + (i) * 0x1000u)
#define CSG_OUT(i)	(CSG_IN(i) + 0x80u)
#define CS_IN(i, j)	(CSG_IN(i) + 0x100u + (j) * 0x180u)
#define CS_OUT(i, j)	(CS_IN(i, j) + 0x80u)

struct mock_csf;

/* ---- pw_os_mock.c, for mock_csf.c. *_locked: the mock lock is held. ---- */
void mock_lock(struct pw_os *os);
void mock_unlock(struct pw_os *os);
struct mock_csf *mock_csf_of(struct pw_os *os);
/* GPU access through AS @as as latched by the last UPDATE: 0 or a Mali fault code. */
int  mock_gpu_rw_locked(struct pw_os *os, unsigned int as, uint64_t va, void *buf, size_t len,
			bool write);
/* MCU access to the shared section (offset relative to SH_VA), through AS0. */
uint32_t mock_sh_r32_locked(struct pw_os *os, uint32_t off);
void mock_sh_w32_locked(struct pw_os *os, uint32_t off, uint32_t v);
void mock_raise_job_locked(struct pw_os *os, uint32_t bits);
void mock_model_error(struct pw_os *os, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 2, 3)))
#endif
	;

/* ---- mock_csf.c, for pw_os_mock.c ---- */
struct mock_csf *mock_csf_create(void);
void mock_csf_destroy(struct mock_csf *csf);
/* MCU (re)boot, stop or GPU soft reset: every CSG/CS is idle again. */
void mock_csf_reset_locked(struct pw_os *os);
/* Doorbell 0 rang with these CSG bits toggled in GLB doorbell_req. */
void mock_csf_glb_doorbell_locked(struct pw_os *os, uint32_t csg_mask);
/* CS doorbell @id (>= 1). */
void mock_csf_doorbell_locked(struct pw_os *os, uint32_t id);

/* ---- (jm-arch) pw_os_mock.c, for mock_jm.c ---- */
struct mock_jm;
struct mock_jm *mock_jm_of(struct pw_os *os);
enum mock_model mock_model_of(struct pw_os *os);
/* Raw register file (no side effects), for the executor's JS_* registers. */
uint32_t mock_reg_get_locked(struct pw_os *os, uint32_t off);
void mock_reg_set_locked(struct pw_os *os, uint32_t off, uint32_t v);
/* MMU fault on @as (like mock_inject_mmu_fault), for GPU accesses that fault. */
void mock_raise_mmu_fault_locked(struct pw_os *os, unsigned int as, uint32_t fault_status,
				 uint64_t va);

/* ---- (jm-arch) mock_jm.c, for pw_os_mock.c (G31 model only) ---- */
struct mock_jm *mock_jm_create(void);
void mock_jm_destroy(struct mock_jm *jm);
/* GPU soft/hard reset: every slot idle, NEXT registers cleared. */
void mock_jm_reset_locked(struct pw_os *os);
/* A write to JOB_CONTROL (0x1010-0x1fff: JS_STATE, THROTTLE, JS_* slot registers; the
 * JOB_INT block itself is modelled by pw_os_mock.c). Return true if the executor handled it
 * (including storing it), false to let pw_os_mock.c store it as a plain register. */
bool mock_jm_reg_write_locked(struct pw_os *os, uint32_t off, uint32_t v);
/* A read from the same range: return true and *v if the executor supplies the value. */
bool mock_jm_reg_read_locked(struct pw_os *os, uint32_t off, uint32_t *v);

#endif /* MOCK_INTERNAL_H */
