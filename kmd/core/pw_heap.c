// SPDX-License-Identifier: MIT
/*
 * pw_heap.c - tiler heaps: TILER_HEAP_CREATE/DESTROY and growth on TILER_OOM.
 *
 * Derived from Linux panthor_heap.c and the TILER_HEAP_* parts of panthor_drv.c (dual
 * GPL-2.0/MIT, used under MIT; Copyright 2023 Collabora ltd.). Line references are to the v7.3
 * sources in references/linux/drivers/gpu/drm/panthor.
 *
 * Differences from panthor:
 *  - panthor hangs the heap-context pool off the VM (panthor_vm::heaps.pool) and destroys it
 *    with the VM (mmu.c:1631). struct pw_uvm belongs to pw_bo.c, so the pools live in the file
 *    (struct pw_heap_file), keyed by the pw_uvm they hold a reference on. After VM_DESTROY the
 *    pool can no longer be reached (TILER_HEAP_DESTROY looks the VM up by id first, like
 *    panthor) and is freed with the file; VM_DESTROY already removed its GPU mappings.
 *  - One mutex per file protects every pool and heap (panthor: a rwsem per pool plus a mutex
 *    per heap).
 */

#include "pw_bo.h"
#include "pw_sched.h"

#define PW_HEAP_CONTEXT_SIZE	32	/* heap.c:22 */
#define PW_MAX_HEAPS_PER_POOL	128	/* heap.c:76 */

/* heap.c:27 struct panthor_heap_chunk_header */
struct pw_heap_chunk_header {
	uint64_t next;			/* GPU VA of the next chunk | chunk_size >> 12 */
	uint32_t unknown[14];		/* MBZ */
};

struct pw_heap_chunk {
	struct pw_heap_chunk *next;
	struct pw_bo *bo;
	uint64_t va;
};

/* heap.c:52 struct panthor_heap */
struct pw_heap {
	struct pw_heap_chunk *chunks;	/* newest first, like list_add() */
	uint32_t chunk_size;
	uint32_t max_chunks;
	uint32_t target_in_flight;
	uint32_t chunk_count;
};

/* heap.c:84 struct panthor_heap_pool */
struct pw_heap_pool {
	struct pw_heap_pool *next;
	struct pw_uvm *uvm;		/* reference */
	struct pw_bo *ctx_bo;		/* the GPU heap contexts */
	uint64_t ctx_va;
	uint8_t *ctx_kva;
	struct pw_heap *heaps[PW_MAX_HEAPS_PER_POOL];
};

struct pw_heap_file {
	struct pw_device *dev;
	struct pw_os_mutex *lock;
	struct pw_heap_pool *pools;
	uint32_t ctx_stride;
};

/* heap.c:106: a heap context is GPU cache-line aligned (GPU_L2_FEATURES_LINE_SIZE). */
static uint32_t heap_ctx_stride(struct pw_device *dev)
{
	uint32_t line = 1u << (dev->gpu_info.l2_features & 0xff);

	if (line > 4096)
		line = 4096;
	return PW_ALIGN_UP((uint32_t)PW_HEAP_CONTEXT_SIZE, line);
}

int pw_heap_file_init(struct pw_file *file, struct pw_heap_file **out)
{
	struct pw_heap_file *hf;

	hf = pw_os_zalloc(file->os, sizeof(*hf));
	if (!hf)
		return PW_ENOMEM;
	hf->dev = file->dev;
	hf->ctx_stride = heap_ctx_stride(file->dev);
	hf->lock = pw_os_mutex_create(file->os);
	if (!hf->lock) {
		pw_os_free(file->os, hf);
		return PW_ENOMEM;
	}
	*out = hf;
	return PW_OK;
}

static void chunk_free(struct pw_heap_pool *pool, struct pw_heap_chunk *c, struct pw_os *os)
{
	if (c->va)
		pw_uvm_unmap_kernel_bo(pool->uvm, c->va);
	pw_bo_put(c->bo);
	pw_os_free(os, c);
}

static void heap_free(struct pw_heap_file *hf, struct pw_heap_pool *pool, struct pw_heap *heap)
{
	struct pw_heap_chunk *c;

	while ((c = heap->chunks)) {
		heap->chunks = c->next;
		chunk_free(pool, c, hf->dev->os);
	}
	pw_os_free(hf->dev->os, heap);
}

/* heap.c:595 panthor_heap_pool_destroy() */
static void pool_free(struct pw_heap_file *hf, struct pw_heap_pool *pool)
{
	for (uint32_t i = 0; i < PW_MAX_HEAPS_PER_POOL; i++) {
		if (pool->heaps[i])
			heap_free(hf, pool, pool->heaps[i]);
	}
	if (pool->ctx_va)
		pw_uvm_unmap_kernel_bo(pool->uvm, pool->ctx_va);
	pw_bo_put(pool->ctx_bo);
	pw_uvm_put(pool->uvm);
	pw_os_free(hf->dev->os, pool);
}

void pw_heap_file_fini(struct pw_heap_file *hf)
{
	struct pw_heap_pool *pool;

	if (!hf)
		return;
	while ((pool = hf->pools)) {
		hf->pools = pool->next;
		pool_free(hf, pool);
	}
	pw_os_mutex_destroy(hf->dev->os, hf->lock);
	pw_os_free(hf->dev->os, hf);
}

static struct pw_heap_pool *pool_find_locked(struct pw_heap_file *hf, struct pw_uvm *uvm)
{
	for (struct pw_heap_pool *p = hf->pools; p; p = p->next) {
		if (p->uvm == uvm)
			return p;
	}
	return NULL;
}

/* heap.c:536 panthor_heap_pool_create(): 128 contexts, NOEXEC, in the VM's kernel range. */
static int pool_get_or_create_locked(struct pw_heap_file *hf, struct pw_uvm *uvm,
				     struct pw_heap_pool **out)
{
	struct pw_device *dev = hf->dev;
	uint64_t size = PW_ALIGN_UP((uint64_t)PW_MAX_HEAPS_PER_POOL * hf->ctx_stride, PW_SZ_4K);
	struct pw_heap_pool *pool = pool_find_locked(hf, uvm);
	int ret;

	if (pool) {
		*out = pool;
		return PW_OK;
	}
	pool = pw_os_zalloc(dev->os, sizeof(*pool));
	if (!pool)
		return PW_ENOMEM;
	pw_uvm_get(uvm);
	pool->uvm = uvm;
	ret = pw_bo_create_kernel(dev, size, PW_MEM_KMAP | PW_MEM_UNCACHED | PW_MEM_ZEROED,
				  &pool->ctx_bo);
	if (ret)
		goto err;
	pool->ctx_kva = pw_os_mem_kva(pw_bo_os_mem(pool->ctx_bo));
	if (!pool->ctx_kva) {
		ret = PW_ENOMEM;
		goto err;
	}
	ret = pw_uvm_map_kernel_bo(uvm, pool->ctx_bo, PW_VM_MAP_NOEXEC, &pool->ctx_va);
	if (ret)
		goto err;
	pool->next = hf->pools;
	hf->pools = pool;
	*out = pool;
	return PW_OK;

err:
	pool_free(hf, pool);
	return ret;
}

/* heap.c:140 panthor_alloc_heap_chunk(). @initial: link to the current first chunk. */
static int heap_alloc_chunk(struct pw_heap_file *hf, struct pw_heap_pool *pool,
			    struct pw_heap *heap, bool initial)
{
	struct pw_device *dev = hf->dev;
	struct pw_heap_chunk_header *hdr;
	struct pw_heap_chunk *c;
	int ret;

	c = pw_os_zalloc(dev->os, sizeof(*c));
	if (!c)
		return PW_ENOMEM;
	/* The CPU only writes the 64-byte header; a write-combined mapping needs no cleaning. */
	ret = pw_bo_create_kernel(dev, heap->chunk_size, PW_MEM_KMAP | PW_MEM_UNCACHED, &c->bo);
	if (ret)
		goto err;
	hdr = pw_os_mem_kva(pw_bo_os_mem(c->bo));
	if (!hdr) {
		ret = PW_ENOMEM;
		goto err;
	}
	ret = pw_uvm_map_kernel_bo(pool->uvm, c->bo, PW_VM_MAP_NOEXEC, &c->va);
	if (ret)
		goto err;

	pw_memset(hdr, 0, sizeof(*hdr));
	if (initial && heap->chunks)
		hdr->next = (heap->chunks->va & PW_GENMASK64(63, 12)) | (heap->chunk_size >> 12);
	pw_os_mem_flush(dev->os, pw_bo_os_mem(c->bo), 0, PW_SZ_4K);

	c->next = heap->chunks;
	heap->chunks = c;
	heap->chunk_count++;
	return PW_OK;

err:
	chunk_free(pool, c, dev->os);
	return ret;
}

/* drv.c:1263 panthor_ioctl_tiler_heap_create() + heap.c:274 panthor_heap_create() */
int pw_ioctl_tiler_heap_create(struct pw_file *file, void *arg)
{
	struct drm_panthor_tiler_heap_create *args = arg;
	struct pw_heap_file *hf = pw_sched_file_heaps(file);
	struct pw_heap_pool *pool;
	struct pw_heap *heap = NULL;
	struct pw_uvm *uvm;
	uint32_t id;
	int ret;

	if (!hf)
		return PW_EINVAL;
	uvm = pw_file_vm_get(file, args->vm_id);
	if (!uvm)
		return PW_EINVAL;

	pw_os_mutex_lock(hf->lock);
	ret = pool_get_or_create_locked(hf, uvm, &pool);
	if (ret)
		goto out_unlock;

	if (!args->initial_chunk_count || args->initial_chunk_count > args->max_chunks ||
	    !PW_IS_ALIGNED(args->chunk_size, PW_SZ_4K) || args->chunk_size < 0x20000u ||
	    args->chunk_size > 0x800000u) {
		ret = PW_EINVAL;
		goto out_unlock;
	}

	heap = pw_os_zalloc(file->os, sizeof(*heap));
	if (!heap) {
		ret = PW_ENOMEM;
		goto out_unlock;
	}
	heap->chunk_size = args->chunk_size;
	heap->max_chunks = args->max_chunks;
	heap->target_in_flight = args->target_in_flight;
	for (uint32_t i = 0; i < args->initial_chunk_count; i++) {
		ret = heap_alloc_chunk(hf, pool, heap, true);
		if (ret)
			goto out_unlock;
	}

	/* xa_alloc(XA_LIMIT(0, MAX_HEAPS_PER_POOL - 1)): -EBUSY when full. */
	for (id = 0; id < PW_MAX_HEAPS_PER_POOL && pool->heaps[id]; id++)
		;
	if (id == PW_MAX_HEAPS_PER_POOL) {
		ret = PW_EBUSY;
		goto out_unlock;
	}
	pw_memset(pool->ctx_kva + (size_t)id * hf->ctx_stride, 0, hf->ctx_stride);
	pw_os_mem_flush(file->os, pw_bo_os_mem(pool->ctx_bo), (size_t)id * hf->ctx_stride,
			hf->ctx_stride);
	pool->heaps[id] = heap;
	args->tiler_heap_ctx_gpu_va = pool->ctx_va + (uint64_t)id * hf->ctx_stride;
	args->first_heap_chunk_gpu_va = heap->chunks->va;
	/* Heap pools are per VM: combine the VM and heap ids into one handle (drv.c:1294). */
	args->handle = (args->vm_id << 16) | id;
	heap = NULL;
	ret = PW_OK;

out_unlock:
	if (heap)
		heap_free(hf, pool, heap);
	pw_os_mutex_unlock(hf->lock);
	pw_uvm_put(uvm);
	return ret;
}

/* drv.c:1306 panthor_ioctl_tiler_heap_destroy() + heap.c:247 panthor_heap_destroy() */
int pw_ioctl_tiler_heap_destroy(struct pw_file *file, void *arg)
{
	struct drm_panthor_tiler_heap_destroy *args = arg;
	struct pw_heap_file *hf = pw_sched_file_heaps(file);
	struct pw_heap_pool *pool;
	struct pw_heap *heap = NULL;
	uint32_t id = args->handle & 0xffffu;
	struct pw_uvm *uvm;
	int ret = PW_OK;

	if (args->pad)
		return PW_EINVAL;
	if (!hf)
		return PW_EINVAL;
	uvm = pw_file_vm_get(file, args->handle >> 16);
	if (!uvm)
		return PW_EINVAL;
	pw_os_mutex_lock(hf->lock);
	pool = pool_find_locked(hf, uvm);
	if (!pool) {
		ret = PW_ENOENT;	/* mmu.c:2144 panthor_vm_get_heap_pool(vm, false) */
	} else if (id >= PW_MAX_HEAPS_PER_POOL || !(heap = pool->heaps[id])) {
		ret = PW_EINVAL;
	} else {
		pool->heaps[id] = NULL;
		heap_free(hf, pool, heap);
	}
	pw_os_mutex_unlock(hf->lock);
	pw_uvm_put(uvm);
	return ret;
}

/* heap.c:432 panthor_heap_grow() */
int pw_heap_grow(struct pw_heap_file *hf, struct pw_uvm *uvm, uint64_t heap_ctx_va,
		 uint32_t renderpasses_in_flight, uint32_t pending_frag_count,
		 uint64_t *new_chunk_va)
{
	struct pw_heap_pool *pool;
	struct pw_heap *heap;
	uint64_t offset;
	uint32_t id;
	int ret;

	(void)pending_frag_count;	/* unused by panthor too */
	*new_chunk_va = 0;
	if (!hf)
		return PW_EINVAL;
	pw_os_mutex_lock(hf->lock);
	pool = pool_find_locked(hf, uvm);
	if (!pool) {
		ret = PW_EINVAL;
		goto out;
	}
	offset = heap_ctx_va - pool->ctx_va;
	id = (uint32_t)offset / hf->ctx_stride;
	if (offset > UINT32_MAX || id >= PW_MAX_HEAPS_PER_POOL || !(heap = pool->heaps[id])) {
		ret = PW_EINVAL;
		goto out;
	}
	/* Too many render passes in flight, or out of chunks: let the FW find another way
	 * (wait for fragment jobs, or the userspace exception handler). */
	if (renderpasses_in_flight > heap->target_in_flight ||
	    heap->chunk_count >= heap->max_chunks) {
		ret = PW_ENOMEM;
		goto out;
	}
	ret = heap_alloc_chunk(hf, pool, heap, false);
	if (ret)
		goto out;
	*new_chunk_va = (heap->chunks->va & PW_GENMASK64(63, 12)) | (heap->chunk_size >> 12);
out:
	pw_os_mutex_unlock(hf->lock);
	return ret;
}
