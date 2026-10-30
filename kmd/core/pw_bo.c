// SPDX-License-Identifier: MIT
/* pw_bo.c - STUB (to be replaced by the real BO / user-VM implementation). */

#include "pw_bo.h"

#define PW_BO_ENOTTY(name) \
	int name(struct pw_file *file, void *arg) { (void)file; (void)arg; return PW_ENOTTY; }
PW_BO_ENOTTY(pw_ioctl_bo_create)
PW_BO_ENOTTY(pw_ioctl_bo_mmap_offset)
PW_BO_ENOTTY(pw_ioctl_bo_set_label)
PW_BO_ENOTTY(pw_ioctl_bo_sync)
PW_BO_ENOTTY(pw_ioctl_bo_query_info)
PW_BO_ENOTTY(pw_ioctl_gem_close)
PW_BO_ENOTTY(pw_ioctl_vm_create)
PW_BO_ENOTTY(pw_ioctl_vm_destroy)
PW_BO_ENOTTY(pw_ioctl_vm_bind)
PW_BO_ENOTTY(pw_ioctl_vm_get_state)

int  pw_bo_file_init(struct pw_file *file) { (void)file; return PW_OK; }
void pw_bo_file_fini(struct pw_file *file) { (void)file; }
int  pw_bo_create_kernel(struct pw_device *dev, uint64_t size, uint32_t mem_flags, struct pw_bo **out)
{ (void)dev; (void)size; (void)mem_flags; *out = NULL; return PW_ENOSYS; }
void pw_bo_get(struct pw_bo *bo) { (void)bo; }
void pw_bo_put(struct pw_bo *bo) { (void)bo; }
struct pw_os_mem *pw_bo_os_mem(struct pw_bo *bo) { (void)bo; return NULL; }
struct pw_bo *pw_file_bo_get(struct pw_file *file, uint32_t handle) { (void)file; (void)handle; return NULL; }
struct pw_bo *pw_file_bo_from_mmap_offset(struct pw_file *file, uint64_t offset, uint64_t *rel)
{ (void)file; (void)offset; (void)rel; return NULL; }
struct pw_uvm *pw_file_vm_get(struct pw_file *file, uint32_t vm_id) { (void)file; (void)vm_id; return NULL; }
void pw_uvm_get(struct pw_uvm *uvm) { (void)uvm; }
void pw_uvm_put(struct pw_uvm *uvm) { (void)uvm; }
struct pw_vm *pw_uvm_vm(struct pw_uvm *uvm) { (void)uvm; return NULL; }
int  pw_uvm_map_kernel_bo(struct pw_uvm *uvm, struct pw_bo *bo, uint32_t map_flags, uint64_t *va_out)
{ (void)uvm; (void)bo; (void)map_flags; *va_out = 0; return PW_ENOSYS; }
void pw_uvm_unmap_kernel_bo(struct pw_uvm *uvm, uint64_t va) { (void)uvm; (void)va; }
int  pw_uvm_activate(struct pw_uvm *uvm) { (void)uvm; return PW_ENOSYS; }
bool pw_uvm_is_unusable(struct pw_uvm *uvm) { (void)uvm; return true; }
