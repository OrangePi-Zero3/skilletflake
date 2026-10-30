// SPDX-License-Identifier: MIT
/* pw_sched_stub.c - placeholder until pw_sched.c/pw_heap.c land: no groups, no heaps. */

#include "pw_sched.h"

int pw_sched_init(struct pw_device *dev) { (void)dev; return PW_OK; }
void pw_sched_fini(struct pw_device *dev) { (void)dev; }
void pw_sched_job_irq(struct pw_device *dev, uint32_t csg_mask) { (void)dev; (void)csg_mask; }
int pw_sched_file_init(struct pw_file *file) { file->sched = NULL; return PW_OK; }
void pw_sched_file_fini(struct pw_file *file) { (void)file; }

#define PW_SCHED_ENOTTY(name) \
	int name(struct pw_file *file, void *arg) { (void)file; (void)arg; return PW_ENOTTY; }
PW_SCHED_ENOTTY(pw_ioctl_group_create)
PW_SCHED_ENOTTY(pw_ioctl_group_destroy)
PW_SCHED_ENOTTY(pw_ioctl_group_submit)
PW_SCHED_ENOTTY(pw_ioctl_group_get_state)
PW_SCHED_ENOTTY(pw_ioctl_tiler_heap_create)
PW_SCHED_ENOTTY(pw_ioctl_tiler_heap_destroy)
