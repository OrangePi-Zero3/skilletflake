// SPDX-License-Identifier: MIT
/*
 * pw_jm_mem.c - job-manager memory: panfrost BOs, per-file address space, HEAP growth,
 * implicit-sync fences (pw_jm_mem.h). Owner: jm-mem.
 *
 * SCAFFOLDING (jm-arch): every entry point is a stub. Lifetime functions succeed so a JM device
 * probes and files open; the BO ioctls return PW_ENOTTY; the fault hook reports "not handled"
 * so faults take the generic unhandled path. The contract for each function is in
 * pw_jm_mem.h, the task list in docs/status/jm-arch.md (engineer B).
 *
 * Behaviour reference: Linux panfrost (GPL-2.0-only, read only, never copy code).
 */

#include "pw_jm_hw.h"
#include "pw_jm_mem.h"

/* ---- lifetime ---- */

int pw_jm_mem_init(struct pw_device *dev)
{
	(void)dev;
	return PW_OK;
}

void pw_jm_mem_fini(struct pw_device *dev)
{
	(void)dev;
}

void pw_jm_mem_release(struct pw_device *dev)
{
	(void)dev;
}

int pw_jm_mem_file_init(struct pw_file *file)
{
	(void)file;
	return PW_OK;	/* TODO(jm-mem): file->jm_mem with the file's pw_jm_vm */
}

void pw_jm_mem_file_fini(struct pw_file *file)
{
	(void)file;
}

/* ---- ioctls ---- */

int pw_jm_ioctl_create_bo(struct pw_file *file, void *arg)
{
	(void)file;
	(void)arg;
	return PW_ENOTTY;
}

int pw_jm_ioctl_mmap_bo(struct pw_file *file, void *arg)
{
	(void)file;
	(void)arg;
	return PW_ENOTTY;
}

int pw_jm_ioctl_get_bo_offset(struct pw_file *file, void *arg)
{
	(void)file;
	(void)arg;
	return PW_ENOTTY;
}

int pw_jm_ioctl_wait_bo(struct pw_file *file, void *arg)
{
	(void)file;
	(void)arg;
	return PW_ENOTTY;
}

int pw_jm_ioctl_madvise(struct pw_file *file, void *arg)
{
	(void)file;
	(void)arg;
	return PW_ENOTTY;
}

int pw_jm_ioctl_set_label_bo(struct pw_file *file, void *arg)
{
	(void)file;
	(void)arg;
	return PW_ENOTTY;
}

int pw_jm_ioctl_sync_bo(struct pw_file *file, void *arg)
{
	(void)file;
	(void)arg;
	return PW_ENOTTY;
}

int pw_jm_ioctl_query_bo_info(struct pw_file *file, void *arg)
{
	(void)file;
	(void)arg;
	return PW_ENOTTY;
}

/* ---- address space ---- */

struct pw_jm_vm *pw_jm_file_vm(struct pw_file *file)
{
	(void)file;
	return NULL;
}

struct pw_jm_vm *pw_jm_vm_get(struct pw_jm_vm *jvm)
{
	return jvm;
}

struct pw_jm_vm *pw_jm_vm_tryget(struct pw_jm_vm *jvm)
{
	return jvm;
}

void pw_jm_vm_put(struct pw_jm_vm *jvm)
{
	(void)jvm;
}

struct pw_vm *pw_jm_vm_mmu(struct pw_jm_vm *jvm)
{
	(void)jvm;
	return NULL;
}

/* ---- BOs ---- */

struct pw_jm_bo *pw_jm_bo_of(struct pw_bo *bo)
{
	(void)bo;
	return NULL;
}

uint64_t pw_jm_bo_va(struct pw_bo *bo)
{
	(void)bo;
	return 0;
}

int pw_jm_bos_lookup(struct pw_file *file, uint64_t handles, uint32_t count,
		     struct pw_bo ***out)
{
	(void)file;
	(void)handles;
	(void)count;
	*out = NULL;
	return PW_ENOTTY;
}

void pw_jm_bos_put(struct pw_device *dev, struct pw_bo **bos, uint32_t count)
{
	(void)dev;
	(void)bos;
	(void)count;
}

/* ---- implicit sync ---- */

void pw_jm_resv_lock(struct pw_device *dev)
{
	(void)dev;
}

void pw_jm_resv_unlock(struct pw_device *dev)
{
	(void)dev;
}

struct pw_fence *pw_jm_bo_fence_get(struct pw_bo *bo)
{
	(void)bo;
	return NULL;
}

void pw_jm_bo_fence_set(struct pw_bo *bo, struct pw_fence *fence)
{
	(void)bo;
	(void)fence;
}

/* ---- faults ---- */

int pw_jm_mmu_fault(struct pw_device *dev, uint32_t as, uint32_t fault_status, uint64_t addr)
{
	(void)dev;
	(void)as;
	(void)fault_status;
	(void)addr;
	return PW_ENOENT;	/* not handled: generic unhandled-fault path */
}
