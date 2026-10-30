/* SPDX-License-Identifier: MIT */
/*
 * pw_bo.h - buffer objects and user VMs (panthor_gem.c / panthor_mmu.c VM side), in pw_bo.c.
 */
#ifndef PW_BO_H
#define PW_BO_H

#include "pw_file.h"
#include "pw_mmu.h"

struct pw_uvm;

struct pw_bo {
	uint32_t refcount;		/* __atomic */
	struct pw_device *dev;
	struct pw_os_mem *mem;
	uint64_t size;			/* page aligned */
	uint32_t flags;			/* DRM_PANTHOR_BO_* */
	struct pw_uvm *exclusive_vm;	/* DRM_PANTHOR_BO_NO_MMAP + exclusive_vm_id, weak */
	uint64_t mmap_offset;		/* fake offset, 0 = none assigned yet */
	char label[64];

	/* (added by jm-arch) Backend extension. @flags keeps its DRM_PANTHOR_BO_* meaning on
	 * both backends (NO_MMAP, WB_MMAP: pw_file_mmap_lookup and pw_bo_get_mmap_offset read
	 * it); the JM memory module keeps its PANFROST_BO_* flags, GPU VA, heap chunks and
	 * implicit-sync fence in @ext (struct pw_jm_bo, pw_jm_mem.h). @release_ext, if set, runs
	 * exactly once from the final pw_bo_put(), before @mem is freed, at passive level in
	 * whatever context dropped the last reference (ioctl, work item, OS-layer munmap). @mem
	 * is NULL for an unbacked BO (pw_bo_alloc with backed == false: JM HEAP BOs). */
	void *ext;
	void (*release_ext)(struct pw_bo *bo);
};

/* ioctl handlers (pw_ioctl_fn) */
int pw_ioctl_bo_create(struct pw_file *file, void *arg);
int pw_ioctl_bo_mmap_offset(struct pw_file *file, void *arg);
int pw_ioctl_bo_set_label(struct pw_file *file, void *arg);
int pw_ioctl_bo_sync(struct pw_file *file, void *arg);
int pw_ioctl_bo_query_info(struct pw_file *file, void *arg);
int pw_ioctl_gem_close(struct pw_file *file, void *arg);		/* DRM core drm_gem_close */
int pw_ioctl_vm_create(struct pw_file *file, void *arg);
int pw_ioctl_vm_destroy(struct pw_file *file, void *arg);
int pw_ioctl_vm_bind(struct pw_file *file, void *arg);
int pw_ioctl_vm_get_state(struct pw_file *file, void *arg);

int  pw_bo_file_init(struct pw_file *file);	/* bos + vms tables */
void pw_bo_file_fini(struct pw_file *file);

/* Kernel BOs (ring buffers, heap chunks, suspend buffers, ...). flags: PW_MEM_* */
int  pw_bo_create_kernel(struct pw_device *dev, uint64_t size, uint32_t mem_flags,
			 struct pw_bo **out);
void pw_bo_get(struct pw_bo *bo);
/* pw_bo_put() is declared in pw_core.h. */
struct pw_bo *pw_file_bo_get(struct pw_file *file, uint32_t handle);	/* ref, or NULL */
/* mmap offset -> BO with a ref (pw_file_mmap_lookup() uses this). */
struct pw_bo *pw_file_bo_from_mmap_offset(struct pw_file *file, uint64_t offset, uint64_t *rel);

/* User VMs */
struct pw_uvm *pw_file_vm_get(struct pw_file *file, uint32_t vm_id);	/* ref, or NULL */
void pw_uvm_get(struct pw_uvm *uvm);
void pw_uvm_put(struct pw_uvm *uvm);
struct pw_vm *pw_uvm_vm(struct pw_uvm *uvm);
/* Map a kernel BO in the kernel-reserved part of the VA space (above the user VA range chosen at
 * VM_CREATE); the VM keeps a BO reference until unmapped/destroyed. */
int  pw_uvm_map_kernel_bo(struct pw_uvm *uvm, struct pw_bo *bo, uint32_t map_flags,
			  uint64_t *va_out);
void pw_uvm_unmap_kernel_bo(struct pw_uvm *uvm, uint64_t va);
/* Bind to an AS slot so groups using it can run (pw_vm_activate); PW_EBUSY if none free. */
int  pw_uvm_activate(struct pw_uvm *uvm);
/* Set when the GPU reported an unrecoverable fault on this VM (VM_GET_STATE -> UNUSABLE). */
bool pw_uvm_is_unusable(struct pw_uvm *uvm);

/* ---- additions (core-mem) ---- */

/* pw_uvm_activate() also fails with PW_EINVAL once the VM is unusable (sticky). */

/* BO_CREATE with an explicit set of accepted DRM_PANTHOR_BO_* flags. pw_ioctl_bo_create() passes
 * the flags the emulated driver version allows (WB_MMAP needs >= 1.7); tests use this to
 * exercise the WB_MMAP path regardless of the reported version. */
int pw_bo_create_user(struct pw_file *file, struct drm_panthor_bo_create *args,
		      uint32_t allowed_flags);
/* True if user CPU mappings of @bo must be cacheable (WB_MMAP, or an IO-coherent GPU). */
bool pw_bo_cpu_cached(struct pw_bo *bo);
/* Kernel VA range of @uvm: [*start, *end). User binds are limited to [0, *start). */
void pw_uvm_kernel_va_range(struct pw_uvm *uvm, uint64_t *start, uint64_t *end);

/* ---- additions (jm-arch): pieces of the panthor BO code the JM backend reuses ---- */

/*
 * New BO, refcount 1, not in any handle table. @mem_flags: PW_MEM_*; @cpu_cached: what
 * pw_bo_cpu_cached() reports (must match PW_MEM_UNCACHED being clear). @backed == false
 * allocates no memory (bo->mem == NULL; the caller manages pages itself, e.g. JM HEAP BOs
 * grown on fault). @size is rounded up to 4 KiB. PW_EINVAL for size 0, PW_ENOMEM.
 * The caller sets bo->flags (DRM_PANTHOR_BO_* meaning), ext/release_ext, then publishes it
 * with pw_handle_alloc(&file->bos, ...), which takes over the reference.
 */
int pw_bo_alloc(struct pw_device *dev, uint64_t size, uint32_t mem_flags, bool cpu_cached,
		bool backed, struct pw_bo **out);
/* Fake mmap offset of @handle, assigned on first use (BO_MMAP_OFFSET minus its pad check):
 * PW_ENOENT (no such handle, or closed concurrently), PW_EPERM (DRM_PANTHOR_BO_NO_MMAP),
 * PW_ENOSPC (offset space full). */
int pw_bo_get_mmap_offset(struct pw_file *file, uint32_t handle, uint64_t *offset);
/* Set @bo's label from a NUL-terminated user string (0 clears it); BO_SET_LABEL minus its pad
 * check and lookup: PW_EFAULT, PW_E2BIG (no NUL within 4096 bytes). Keeps 63 bytes. */
int pw_bo_set_label_from_user(struct pw_file *file, struct pw_bo *bo, uint64_t user_label);

#endif /* PW_BO_H */
