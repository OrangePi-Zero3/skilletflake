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

#endif /* PW_BO_H */
