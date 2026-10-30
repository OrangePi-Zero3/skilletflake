// SPDX-License-Identifier: MIT
/*
 * mock_jm.c - job-manager executor of the mock G31 (mock_config.model = MOCK_MODEL_G31).
 *
 * PLACEHOLDER (jm-arch). pw_os_mock.c already routes every JOB_CONTROL access above the
 * JOB_INT block (0x1010-0x1fff: JS_STATE, THROTTLE and the JS_* slot registers) here, under the
 * mock lock, and calls mock_jm_reset_locked() on a GPU reset. For now writes and reads fall
 * through to the plain register file, so a START never completes.
 *
 * To be written by the hardware engineer (jm-hw, docs/status/jm-arch.md task A6): per slot the
 * current + NEXT job, JS_COMMAND_NEXT = START latching HEAD/AFFINITY/CONFIG_NEXT, a walker of
 * the job chain through the AS in JS_CONFIG (Mesa src/panfrost/genxml/v7.xml "Job Header":
 * exception status, job type, next job pointer; WRITE_VALUE and NULL jobs executed, CACHE_FLUSH
 * counted, vertex/tiler/fragment/compute completed without running shaders, the descriptor's
 * exception status written back unless DISABLE_DESCRIPTOR_WR_BK), JOB_INT DONE/ERR, JS_STATUS,
 * JS_HEAD/JS_TAIL, JS_STATE, SOFT/HARD_STOP (with the _0/_1 JOB_CHAIN_FLAG variants), GPU
 * faults raised as MMU faults (mock_raise_mmu_fault_locked) that fail the job, and fault
 * injection hooks for the tests. Mesa's genxml is MIT; Linux panfrost (GPL-2.0-only) is a
 * behaviour reference only.
 */

#include <stdlib.h>
#include <string.h>

#include "mock_internal.h"

struct mock_jm {
	struct mock_js_stats js[16];
};

struct mock_jm *mock_jm_create(void)
{
	return calloc(1, sizeof(struct mock_jm));
}

void mock_jm_destroy(struct mock_jm *jm)
{
	free(jm);
}

void mock_jm_reset_locked(struct pw_os *os)
{
	(void)os;
}

bool mock_jm_reg_write_locked(struct pw_os *os, uint32_t off, uint32_t v)
{
	(void)os;
	(void)off;
	(void)v;
	return false;
}

bool mock_jm_reg_read_locked(struct pw_os *os, uint32_t off, uint32_t *v)
{
	(void)os;
	(void)off;
	(void)v;
	return false;
}

void mock_jm_js_stats(struct pw_os *os, unsigned int js, struct mock_js_stats *st)
{
	memset(st, 0, sizeof(*st));
	mock_lock(os);
	if (js < 16)
		*st = mock_jm_of(os)->js[js];
	mock_unlock(os);
}
