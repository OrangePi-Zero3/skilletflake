// SPDX-License-Identifier: MIT
/*
 * pw_jm_hw.c - job-manager GPU hardware (pw_jm_hw.h). Owner: jm-hw.
 *
 * SCAFFOLDING (jm-arch): every entry point is a stub. Lifetime functions succeed without
 * touching the hardware so a JM device probes and files open; GET_PARAM returns PW_ENOTTY;
 * the slot/AS/reset helpers do nothing or fail with PW_ENOTTY. The contract for each function
 * is in pw_jm_hw.h, the task list in docs/status/jm-arch.md (engineer A).
 *
 * Behaviour reference: Linux panfrost (GPL-2.0-only, read only, never copy code).
 */

#include "pw_jm_hw.h"
#include "pw_jm_mem.h"
#include "pw_jm_sched.h"

/* ---- device lifetime ---- */

int pw_jm_hw_init(struct pw_device *dev)
{
	(void)dev;
	return PW_OK;	/* TODO(jm-hw): features, quirks, shader/tiler power, job IRQ, AS table */
}

void pw_jm_hw_fini(struct pw_device *dev)
{
	(void)dev;
}

void pw_jm_hw_release(struct pw_device *dev)
{
	(void)dev;
}

/* ---- GET_PARAM ---- */

const struct pw_jm_gpu_info *pw_jm_gpu_info(struct pw_device *dev)
{
	(void)dev;
	return NULL;	/* TODO(jm-hw): valid after pw_jm_hw_init() */
}

int pw_jm_ioctl_get_param(struct pw_file *file, void *arg)
{
	(void)file;
	(void)arg;
	return PW_ENOTTY;
}

/* ---- job slots ---- */

void pw_jm_slot_submit(struct pw_device *dev, uint32_t js, uint64_t jc, uint32_t as,
		       uint32_t flush_id, bool chain_flag)
{
	(void)dev;
	(void)js;
	(void)jc;
	(void)as;
	(void)flush_id;
	(void)chain_flag;
}

bool pw_jm_slot_next_free(struct pw_device *dev, uint32_t js)
{
	(void)dev;
	(void)js;
	return false;
}

void pw_jm_slot_stop(struct pw_device *dev, uint32_t js, bool hard, bool chain_flag)
{
	(void)dev;
	(void)js;
	(void)hard;
	(void)chain_flag;
}

void pw_jm_slot_cancel_next(struct pw_device *dev, uint32_t js)
{
	(void)dev;
	(void)js;
}

uint32_t pw_jm_slot_status(struct pw_device *dev, uint32_t js)
{
	(void)dev;
	(void)js;
	return 0;
}

uint64_t pw_jm_slot_head(struct pw_device *dev, uint32_t js)
{
	(void)dev;
	(void)js;
	return 0;
}

uint64_t pw_jm_slot_tail(struct pw_device *dev, uint32_t js)
{
	(void)dev;
	(void)js;
	return 0;
}

uint32_t pw_jm_latest_flush_id(struct pw_device *dev)
{
	(void)dev;
	return 0;
}

void pw_jm_cycle_counter_get(struct pw_device *dev)
{
	(void)dev;
}

void pw_jm_cycle_counter_put(struct pw_device *dev)
{
	(void)dev;
}

const char *pw_jm_exception_name(uint32_t code)
{
	return pw_exception_name(code);	/* TODO(jm-hw): JM meanings of 0x01-0x03, 0x40-0x4b */
}

/* ---- job IRQ ---- */

void pw_jm_job_irq_handler(struct pw_device *dev, uint32_t status)
{
	(void)dev;
	(void)status;
}

/* ---- recovery ---- */

int pw_jm_hw_reset(struct pw_device *dev)
{
	(void)dev;
	return PW_ENOTTY;
}

/* ---- AS slots ---- */

int pw_jm_as_get(struct pw_device *dev, struct pw_jm_vm *jvm)
{
	(void)dev;
	(void)jvm;
	return PW_ENOTTY;
}

void pw_jm_as_put(struct pw_device *dev, struct pw_jm_vm *jvm)
{
	(void)dev;
	(void)jvm;
}

void pw_jm_as_release(struct pw_device *dev, struct pw_jm_vm *jvm)
{
	(void)dev;
	(void)jvm;
}

struct pw_jm_vm *pw_jm_as_vm_get(struct pw_device *dev, uint32_t as)
{
	(void)dev;
	(void)as;
	return NULL;
}
