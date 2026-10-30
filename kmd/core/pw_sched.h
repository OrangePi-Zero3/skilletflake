/* SPDX-License-Identifier: MIT */
/*
 * pw_sched.h - CSF scheduling: groups, queues, job submission, tiler heaps (panthor_sched.c,
 * panthor_heap.c). Implemented by pw_sched.c (groups, jobs, CSG slots, IRQs) and pw_heap.c
 * (tiler heaps).
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

/* ---- additions (core-sched) ---- */

/* Job timeout (panthor JOB_TIMEOUT_MS, sched.c:94: 5000 ms). For tests and bring-up; takes
 * effect for timers armed afterwards. No-op without a scheduler. */
void pw_sched_set_job_timeout(struct pw_device *dev, uint32_t timeout_ms);

/* pw_heap.c <-> pw_sched.c: per-file tiler heap state (one heap-context pool per VM, like
 * panthor_vm::heaps.pool) and TILER_OOM growth (heap.c:432 panthor_heap_grow). */
struct pw_heap_file;
struct pw_uvm;
int  pw_heap_file_init(struct pw_file *file, struct pw_heap_file **out);
void pw_heap_file_fini(struct pw_heap_file *hf);
/* The heap state of @file (owned by its pw_sched_file). */
struct pw_heap_file *pw_sched_file_heaps(struct pw_file *file);
/* PW_OK and *new_chunk_va (chunk VA | chunk_size >> 12, the format the FW wants in heap_start/
 * heap_end), PW_ENOMEM (limits reached or allocation failure: the FW must find memory another
 * way), PW_EINVAL (no pool for @uvm, or @heap_ctx_va is not one of its heap contexts). */
int  pw_heap_grow(struct pw_heap_file *hf, struct pw_uvm *uvm, uint64_t heap_ctx_va,
		  uint32_t renderpasses_in_flight, uint32_t pending_frag_count,
		  uint64_t *new_chunk_va);

#endif /* PW_SCHED_H */
