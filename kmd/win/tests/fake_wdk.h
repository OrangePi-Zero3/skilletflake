/* SPDX-License-Identifier: MIT */
/* Test-side controls of the fake WDK (fake_wdk.c). */
#ifndef FAKE_WDK_H
#define FAKE_WDK_H

#include <ntddk.h>
#include <wdf.h>

/* Processes: the test runs as fake_proc_a by default; a thread may switch with
 * fake_set_current_process() (simulates a handle used from another process). */
extern struct fake_process fake_proc_a, fake_proc_b;
void fake_set_current_process(PEPROCESS p);

/* Queue a "user APC" to the thread with id @tid (PsGetCurrentThreadId() value): its current or
 * next UserMode alertable wait returns STATUS_USER_APC (once). */
void fake_queue_user_apc(HANDLE tid);

/* A user-mode event "handle" for SYNCOBJ_EVENT. refs starts at 1 (the handle). */
PKEVENT fake_user_event_create(void);
int fake_user_event_refs(PKEVENT e);
BOOLEAN fake_event_is_set(PKEVENT e);
void fake_user_event_close(PKEVENT e);

/* Statistics. */
extern long fake_user_maps_live;	/* MmMapLockedPagesSpecifyCache(UserMode) minus unmaps */
extern long fake_unmap_wrong_process;	/* user unmaps done outside the mapping's process */
extern long fake_attach_count;		/* KeStackAttachProcess calls */
extern long fake_work_items_live;	/* queued + running IO_WORKITEM routines */
void fake_wait_work_items_idle(void);

/* Wrap a context in a WDF object. */
struct fake_wdf_object *fake_wdf_object_new(size_t ctx_size);
void fake_wdf_object_free(struct fake_wdf_object *o);

uint64_t fake_now_ns(void);

#endif /* FAKE_WDK_H */
