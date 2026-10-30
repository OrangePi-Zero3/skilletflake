/* SPDX-License-Identifier: MIT */
/*
 * pw_sched.h - CSF scheduling: groups, queues, job submission, tiler heaps (panthor_sched.c,
 * panthor_heap.c). Implemented by pw_sched.c / pw_heap.c (milestone M3-M5); until then
 * pw_sched_stub.c provides ENOTTY handlers.
 */
#ifndef PW_SCHED_H
#define PW_SCHED_H

#include "pw_file.h"

/* Device level: after FW boot / before FW teardown. No-ops when the FW isn't booted. */
int  pw_sched_init(struct pw_device *dev);
void pw_sched_fini(struct pw_device *dev);
/* Called from the job IRQ bottom half with the CSG bits (JOB_INT bits 0..30) that fired. */
void pw_sched_job_irq(struct pw_device *dev, uint32_t csg_mask);

int  pw_sched_file_init(struct pw_file *file);
void pw_sched_file_fini(struct pw_file *file);

/* ioctl handlers (pw_ioctl_fn), panthor structs */
int pw_ioctl_group_create(struct pw_file *file, void *arg);
int pw_ioctl_group_destroy(struct pw_file *file, void *arg);
int pw_ioctl_group_submit(struct pw_file *file, void *arg);
int pw_ioctl_group_get_state(struct pw_file *file, void *arg);
int pw_ioctl_tiler_heap_create(struct pw_file *file, void *arg);
int pw_ioctl_tiler_heap_destroy(struct pw_file *file, void *arg);

#endif /* PW_SCHED_H */
