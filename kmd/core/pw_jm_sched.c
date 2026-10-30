// SPDX-License-Identifier: MIT
/*
 * pw_jm_sched.c - job-manager scheduling: SUBMIT, JM contexts, slot queues, completion,
 * timeouts, reset policy (pw_jm_sched.h). Owner: jm-sched.
 *
 * SCAFFOLDING (jm-arch): every entry point is a stub. Lifetime functions succeed so a JM device
 * probes and files open; SUBMIT and JM_CTX_* return PW_ENOTTY. The contract for each function
 * is in pw_jm_sched.h, the task list in docs/status/jm-arch.md (engineer C).
 *
 * Behaviour reference: Linux panfrost (GPL-2.0-only, read only, never copy code).
 */

#include "pw_jm_mem.h"
#include "pw_jm_sched.h"
#include "pw_sync.h"

/* ---- device lifetime ---- */

int pw_jm_sched_init(struct pw_device *dev)
{
	(void)dev;
	return PW_OK;
}

void pw_jm_sched_fini(struct pw_device *dev)
{
	(void)dev;
}

void pw_jm_sched_release(struct pw_device *dev)
{
	(void)dev;
}

void pw_jm_sched_set_job_timeout(struct pw_device *dev, uint32_t timeout_ms)
{
	(void)dev;
	(void)timeout_ms;
}

/* ---- per file ---- */

int pw_jm_sched_file_init(struct pw_file *file)
{
	(void)file;
	return PW_OK;	/* TODO(jm-sched): file->jm_sched with the default context 0 */
}

void pw_jm_sched_file_fini(struct pw_file *file)
{
	(void)file;
}

/* ---- ioctls ---- */

int pw_jm_ioctl_submit(struct pw_file *file, void *arg)
{
	(void)file;
	(void)arg;
	return PW_ENOTTY;
}

int pw_jm_ioctl_jm_ctx_create(struct pw_file *file, void *arg)
{
	(void)file;
	(void)arg;
	return PW_ENOTTY;
}

int pw_jm_ioctl_jm_ctx_destroy(struct pw_file *file, void *arg)
{
	(void)file;
	(void)arg;
	return PW_ENOTTY;
}

/* ---- from pw_jm_hw.c ---- */

void pw_jm_sched_job_irq(struct pw_device *dev, const struct pw_jm_irq_event *ev)
{
	(void)dev;
	(void)ev;
}
