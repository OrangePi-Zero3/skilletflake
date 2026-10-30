// SPDX-License-Identifier: MIT
/*
 * pw_fw.c - CSF firmware: image parsing, MCU VM + section loading, MCU boot, interface
 * discovery, global interface init, req/ack helpers, doorbells and GLB_PING.
 *
 * Derived from Linux panthor_fw.c / panthor_fw.h (dual GPL-2.0/MIT, used under MIT;
 * Copyright 2023 Collabora ltd.). Line references are to the v7.3 sources in references/linux.
 *
 * The image is untrusted input: every offset/size read from it is bounds-checked before use
 * (panthor's checks plus a few more, see pw_fw_parse()), and the interface blocks the MCU writes
 * into the shared section are snapshotted and checked against the section bounds.
 */

#include "pw_fw.h"
#include "pw_mmu.h"

#define CSF_FW_NAME			"mali_csffw.bin"
#define PW_FW_BOOT_TIMEOUT_MS		1000	/* fw.c:1140 */
#define PW_FW_PING_TIMEOUT_MS		100	/* fw.c:1452 */
#define PROGRESS_TIMEOUT_CYCLES		(5ull * 500 * 1024 * 1024)	/* fw.c:34 */
#define PROGRESS_TIMEOUT_SCALE_SHIFT	10
#define IDLE_HYSTERESIS_US		800
#define PWROFF_HYSTERESIS_US		10000
/*
 * Arch timer (CNTFRQ) rate for FW timeouts. panthor reads it with arch_timer_get_cntfrq()
 * (fw.c:420); the core has no access to it, and RK3588's system counter runs at 24 MHz.
 * TODO: take it from the OS layer (see "Requested contract changes" in docs/status/kmd-core.md).
 */
#define PW_DEFAULT_TIMER_HZ		24000000u

/* ------------------------------------------------------------------------------------------ */
/* image parser                                                                               */
/* ------------------------------------------------------------------------------------------ */

struct fw_iter {
	const uint8_t *data;
	size_t size;
	size_t offset;
};

/* fw.c:448-462 */
static int iter_read(struct pw_os *os, struct fw_iter *it, void *out, size_t size)
{
	size_t new_offset = it->offset + size;

	if (new_offset > it->size || new_offset < it->offset) {
		pw_os_log(os, PW_LOG_ERR, "fw: image too small (need %zu bytes at 0x%zx of 0x%zx)",
			  size, it->offset, it->size);
		return PW_EINVAL;
	}
	pw_memcpy(out, it->data + it->offset, size);
	it->offset = new_offset;
	return PW_OK;
}

/* fw.c:464-480 */
static int sub_iter_init(struct pw_os *os, struct fw_iter *it, struct fw_iter *sub, size_t size)
{
	size_t new_offset = it->offset + size;

	if (new_offset > it->size || new_offset < it->offset) {
		pw_os_log(os, PW_LOG_ERR, "fw: entry at 0x%zx too long (0x%zx bytes)",
			  it->offset, size);
		return PW_EINVAL;
	}
	sub->offset = 0;
	sub->data = it->data + it->offset;
	sub->size = size;
	it->offset = new_offset;
	return PW_OK;
}

/* Validation part of fw.c:538-604 (panthor_fw_load_section_entry). */
static int parse_iface(struct pw_os *os, const uint8_t *image, size_t image_size,
		       struct fw_iter *eit, uint32_t entry_offset, const struct pw_fw_parse_ops *ops,
		       void *ctx, struct pw_fw_image_info *info)
{
	struct pw_fw_binary_section_entry_hdr hdr;
	struct pw_fw_iface_entry e;
	int ret;

	ret = iter_read(os, eit, &hdr, sizeof(hdr));
	if (ret)
		return ret;

	if (hdr.data.end < hdr.data.start) {
		pw_os_log(os, PW_LOG_ERR, "fw: corrupted, data.end < data.start (0x%x < 0x%x)",
			  hdr.data.end, hdr.data.start);
		return PW_EINVAL;
	}
	if (hdr.va.end < hdr.va.start) {
		pw_os_log(os, PW_LOG_ERR, "fw: corrupted, va.end < va.start (0x%x < 0x%x)",
			  hdr.va.end, hdr.va.start);
		return PW_EINVAL;
	}
	if (hdr.data.end > image_size) {
		pw_os_log(os, PW_LOG_ERR, "fw: truncated? data_end=0x%x > size=0x%zx",
			  hdr.data.end, image_size);
		return PW_EINVAL;
	}
	if (!PW_IS_ALIGNED(hdr.va.start, PW_FW_PAGE_SIZE) ||
	    !PW_IS_ALIGNED(hdr.va.end, PW_FW_PAGE_SIZE)) {
		pw_os_log(os, PW_LOG_ERR, "fw: VAs not page aligned: 0x%x-0x%x",
			  hdr.va.start, hdr.va.end);
		return PW_EINVAL;
	}
	if (hdr.flags & ~CSF_FW_BINARY_IFACE_ENTRY_SUPPORTED_FLAGS) {
		pw_os_log(os, PW_LOG_ERR, "fw: interface with unsupported flags 0x%x", hdr.flags);
		return PW_EINVAL;
	}
	if (hdr.flags & CSF_FW_BINARY_IFACE_ENTRY_PROT) {
		pw_os_log(os, PW_LOG_WARN, "fw: protected mode section at 0x%x not supported, ignoring",
			  hdr.va.start);
		info->n_iface_prot++;
		return PW_OK;
	}
	if (hdr.va.start == CSF_MCU_SHARED_REGION_START &&
	    !(hdr.flags & CSF_FW_BINARY_IFACE_ENTRY_SHARED)) {
		pw_os_log(os, PW_LOG_ERR, "fw: interface at 0x%llx must be shared",
			  (unsigned long long)CSF_MCU_SHARED_REGION_START);
		return PW_EINVAL;
	}
	if (hdr.data.end - hdr.data.start > hdr.va.end - hdr.va.start) {
		pw_os_log(os, PW_LOG_ERR, "fw: section data exceeds section size");
		return PW_EINVAL;
	}
	if (hdr.va.end == hdr.va.start)
		return PW_OK;
	/* Extra: bound what a malformed image can make us allocate. */
	if (hdr.va.end - hdr.va.start > PW_FW_MAX_SECTION_SIZE ||
	    info->total_size + (hdr.va.end - hdr.va.start) > PW_FW_MAX_TOTAL_SIZE) {
		pw_os_log(os, PW_LOG_ERR, "fw: section 0x%x-0x%x too large", hdr.va.start, hdr.va.end);
		return PW_EINVAL;
	}
	info->total_size += hdr.va.end - hdr.va.start;

	if (hdr.va.start == CSF_MCU_SHARED_REGION_START) {
		/* Extra checks: exactly one shared region, big enough for the global control
		 * block plus at least the group control offset. */
		if (info->has_shared) {
			pw_os_log(os, PW_LOG_ERR, "fw: duplicate shared interface section");
			return PW_EINVAL;
		}
		if (hdr.va.end - hdr.va.start < CSF_GROUP_CONTROL_OFFSET + PW_FW_PAGE_SIZE) {
			pw_os_log(os, PW_LOG_ERR, "fw: shared section too small (0x%x)",
				  hdr.va.end - hdr.va.start);
			return PW_EINVAL;
		}
		info->has_shared = true;
	}

	e.flags = hdr.flags;
	e.va_start = hdr.va.start;
	e.va_end = hdr.va.end;
	e.data_start = hdr.data.start;
	e.data_end = hdr.data.end;
	e.name = eit->data + eit->offset;
	e.name_len = (uint32_t)(eit->size - eit->offset);
	e.entry_offset = entry_offset;
	info->n_iface_loaded++;
	return ops && ops->iface ? ops->iface(ctx, &e, image) : PW_OK;
}

/* fw.c:606-640 */
static void parse_build_info(struct pw_os *os, const uint8_t *image, size_t image_size,
			     struct fw_iter *eit, struct pw_fw_image_info *info)
{
	static const char git_sha_header[] = "git_sha: ";
	const size_t header_len = sizeof(git_sha_header) - 1;
	struct pw_fw_build_info_hdr hdr;

	if (iter_read(os, eit, &hdr, sizeof(hdr)))
		return;	/* not fatal: we don't need the build info */
	if (hdr.meta_start > image_size || hdr.meta_size > image_size - hdr.meta_start ||
	    hdr.meta_size <= header_len) {
		pw_os_log(os, PW_LOG_WARN, "fw: build info corrupt");
		return;
	}
	if (pw_memcmp(git_sha_header, image + hdr.meta_start, header_len))
		return;
	if (image[hdr.meta_start + hdr.meta_size - 1] != '\0') {
		pw_os_log(os, PW_LOG_WARN, "fw: git sha is not NUL terminated");
		return;
	}
	pw_strlcpy(info->git_sha, (const char *)image + hdr.meta_start + header_len,
		   sizeof(info->git_sha));
}

/* fw.c:642-683 (entry dispatch) and fw.c:685-747 (header), without side effects. */
int pw_fw_parse(struct pw_os *os, const void *data, size_t size, const struct pw_fw_parse_ops *ops,
		void *ctx, struct pw_fw_image_info *info)
{
	struct fw_iter it = { data, size, 0 };
	struct pw_fw_binary_hdr hdr;
	int ret;

	pw_memset(info, 0, sizeof(*info));
	if (!data)
		return PW_EINVAL;

	ret = iter_read(os, &it, &hdr, sizeof(hdr));
	if (ret)
		return ret;
	if (hdr.magic != CSF_FW_BINARY_HEADER_MAGIC) {
		pw_os_log(os, PW_LOG_ERR, "fw: invalid magic 0x%08x", hdr.magic);
		return PW_EINVAL;
	}
	if (hdr.major != CSF_FW_BINARY_HEADER_MAJOR_MAX) {
		pw_os_log(os, PW_LOG_ERR, "fw: unsupported binary header version %u.%u (expected %u.x)",
			  hdr.major, hdr.minor, CSF_FW_BINARY_HEADER_MAJOR_MAX);
		return PW_EINVAL;
	}
	if (hdr.size > size) {
		pw_os_log(os, PW_LOG_ERR, "fw: image truncated (header says 0x%x, have 0x%zx)",
			  hdr.size, size);
		return PW_EINVAL;
	}
	info->major = hdr.major;
	info->minor = hdr.minor;
	info->version_hash = hdr.version_hash;
	info->hdr_size = hdr.size;
	it.size = hdr.size;

	while (it.offset < it.size) {
		struct fw_iter eit;
		uint32_t ehdr, esize, entry_offset = (uint32_t)it.offset;

		if (++info->n_entries > PW_FW_MAX_ENTRIES) {
			pw_os_log(os, PW_LOG_ERR, "fw: too many entries");
			return PW_EINVAL;
		}
		ret = iter_read(os, &it, &ehdr, sizeof(ehdr));
		if (ret)
			return ret;
		esize = CSF_FW_BINARY_ENTRY_SIZE(ehdr);
		if ((it.offset % sizeof(uint32_t)) || (esize % sizeof(uint32_t))) {
			pw_os_log(os, PW_LOG_ERR, "fw: entry not 32-bit aligned, offset=0x%x size=0x%x",
				  entry_offset, esize);
			return PW_EINVAL;
		}
		/* Extra: a zero-sized entry would underflow below (panthor relies on the size_t
		 * wrap being caught by the overflow check). */
		if (esize < sizeof(ehdr)) {
			pw_os_log(os, PW_LOG_ERR, "fw: entry at 0x%x has size %u", entry_offset, esize);
			return PW_EINVAL;
		}
		ret = sub_iter_init(os, &it, &eit, esize - sizeof(ehdr));
		if (ret)
			return ret;

		switch (CSF_FW_BINARY_ENTRY_TYPE(ehdr)) {
		case CSF_FW_BINARY_ENTRY_TYPE_IFACE:
			info->n_iface++;
			ret = parse_iface(os, data, size, &eit, entry_offset, ops, ctx, info);
			if (ret)
				return ret;
			continue;
		case CSF_FW_BINARY_ENTRY_TYPE_BUILD_INFO_METADATA:
			parse_build_info(os, data, size, &eit, info);
			continue;
		case CSF_FW_BINARY_ENTRY_TYPE_CONFIG:
			info->n_config++;
			continue;
		case CSF_FW_BINARY_ENTRY_TYPE_TRACE_BUFFER:
			info->n_trace++;
			continue;
		case CSF_FW_BINARY_ENTRY_TYPE_FUTF_TEST:
		case CSF_FW_BINARY_ENTRY_TYPE_TIMELINE_METADATA:
			info->n_other++;
			continue;
		default:
			break;
		}
		if (ehdr & CSF_FW_BINARY_ENTRY_OPTIONAL) {
			info->n_other++;
			continue;
		}
		pw_os_log(os, PW_LOG_ERR, "fw: unsupported non-optional entry type %u",
			  CSF_FW_BINARY_ENTRY_TYPE(ehdr));
		return PW_EINVAL;
	}

	if (!info->has_shared) {
		pw_os_log(os, PW_LOG_ERR, "fw: shared interface region not found");
		return PW_EINVAL;
	}
	return PW_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* section loading                                                                            */
/* ------------------------------------------------------------------------------------------ */

/* fw.c:482-504: copy the initial data and zero the rest. Deviation: we always zero the tail
 * (panthor only does it for ZERO sections but gets zeroed shmem pages anyway). */
static void fw_init_section_mem(struct pw_device *dev, struct pw_fw_section *sec)
{
	uint8_t *kva = pw_os_mem_kva(sec->mem);

	if (sec->data_size)
		pw_memcpy(kva, sec->data, sec->data_size);
	pw_memset(kva + sec->data_size, 0, sec->size - sec->data_size);
	/* dma_sync_sgtable_for_device() equivalent (fw.c:595) */
	pw_os_mem_flush(dev->os, sec->mem, 0, sec->size);
}

/* The bytes after the section header are a name in some images; arch10.8 has non-text data
 * there. Keep it only if it is printable ASCII up to the first NUL. */
static void sanitize_name(char *dst, size_t dst_size, const uint8_t *src, uint32_t len)
{
	size_t n = 0;

	for (uint32_t i = 0; i < len && src[i]; i++) {
		if (src[i] < 0x20 || src[i] >= 0x7f) {
			n = 0;
			break;
		}
		if (n + 1 < dst_size)
			dst[n++] = (char)src[i];
	}
	dst[n] = '\0';
}

/* Allocation part of fw.c:538-604. */
static int fw_load_section(void *ctx, const struct pw_fw_iface_entry *e, const uint8_t *image)
{
	struct pw_device *dev = ctx;
	struct pw_fw *fw = &dev->fw;
	uint32_t cache_mode = e->flags & CSF_FW_BINARY_IFACE_ENTRY_CACHE_MODE_MASK;
	uint32_t map_flags = 0;
	struct pw_fw_section *sec;
	int ret;

	if (fw->nsections >= PW_FW_MAX_SECTIONS) {
		pw_err(dev, "fw: too many sections");
		return PW_ENOSPC;
	}
	sec = &fw->sections[fw->nsections++];
	sec->flags = e->flags;
	sec->va = e->va_start;
	sec->size = e->va_end - e->va_start;
	sec->data_size = e->data_end - e->data_start;
	sanitize_name(sec->name, sizeof(sec->name), e->name, e->name_len);

	if (sec->data_size) {
		sec->data = pw_os_zalloc(dev->os, sec->data_size);
		if (!sec->data)
			return PW_ENOMEM;
		pw_memcpy(sec->data, image + e->data_start, sec->data_size);
	}

	/* On a non-coherent device panthor CPU-maps all kernel BOs write-combined
	 * (should_map_wc(), panthor_gem.c:89), which also makes the shared interface safe to
	 * poke without cache maintenance. We still clean after init, like fw.c:595. */
	ret = pw_os_mem_alloc(dev->os, sec->size, PW_MEM_KMAP | PW_MEM_UNCACHED, &sec->mem);
	if (ret)
		return ret;
	if (!pw_os_mem_kva(sec->mem) || pw_os_mem_size(sec->mem) < sec->size)
		return PW_ENOMEM;
	fw_init_section_mem(dev, sec);

	if (!(e->flags & CSF_FW_BINARY_IFACE_ENTRY_WR))
		map_flags |= PW_VM_MAP_READONLY;
	if (!(e->flags & CSF_FW_BINARY_IFACE_ENTRY_EX))
		map_flags |= PW_VM_MAP_NOEXEC;
	/* fw.c:578-586: *_COHERENT cache modes are mapped non-cacheable for now. */
	if (cache_mode != CSF_FW_BINARY_IFACE_ENTRY_CACHE_MODE_CACHED)
		map_flags |= PW_VM_MAP_UNCACHED;

	ret = pw_vm_map_mem(fw->vm, sec->va, sec->mem, 0, sec->size, map_flags);
	if (ret) {
		pw_err(dev, "fw: mapping section 0x%x-0x%x failed (%d)", sec->va,
		       sec->va + sec->size, ret);
		return ret;
	}
	sec->mapped = true;

	pw_dbg(dev, "fw: section 0x%08x-0x%08x flags 0x%08x data 0x%x '%s'", sec->va,
	       sec->va + sec->size, sec->flags, sec->data_size, sec->name);

	if (sec->va == CSF_MCU_SHARED_REGION_START)
		fw->shared = sec;
	return PW_OK;
}

static void fw_free_sections(struct pw_device *dev)
{
	struct pw_fw *fw = &dev->fw;

	for (uint32_t i = 0; i < fw->nsections; i++) {
		struct pw_fw_section *sec = &fw->sections[i];

		if (sec->mem)
			pw_os_mem_free(dev->os, sec->mem);
		if (sec->data)
			pw_os_free(dev->os, sec->data);
		pw_memset(sec, 0, sizeof(*sec));
	}
	fw->nsections = 0;
	fw->shared = NULL;
}

static int fw_load(struct pw_device *dev)
{
	static const struct pw_fw_parse_ops ops = { .iface = fw_load_section };
	struct pw_fw_image_info info;
	const void *data = NULL;
	size_t size = 0;
	int ret;

	ret = pw_os_firmware_load(dev->os, dev->fw_name, &data, &size);
	if (ret || !data) {
		pw_err(dev, "failed to load firmware image '%s' (%d)", dev->fw_name, ret);
		return ret ? ret : PW_ENOENT;
	}

	ret = pw_fw_parse(dev->os, data, size, &ops, dev, &info);
	pw_os_firmware_release(dev->os, data);
	if (ret) {
		pw_err(dev, "firmware '%s' rejected (%d)", dev->fw_name, ret);
		return ret;
	}
	if (!dev->fw.shared)
		return PW_EINVAL;

	dev->fw.hdr_major = info.major;
	dev->fw.hdr_minor = info.minor;
	dev->fw.version_hash = info.version_hash;
	pw_strlcpy(dev->fw.git_sha, info.git_sha, sizeof(dev->fw.git_sha));
	pw_info(dev, "firmware '%s': header v%u.%u hash 0x%x, %u sections (%u protected skipped)%s%s",
		dev->fw_name, info.major, info.minor, info.version_hash, info.n_iface_loaded,
		info.n_iface_prot, info.git_sha[0] ? ", git sha " : "", info.git_sha);
	return PW_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* interfaces                                                                                 */
/* ------------------------------------------------------------------------------------------ */

/* fw.c:778-799 (iface_fw_to_cpu_addr) */
static void *iface_fw_to_cpu_addr(struct pw_device *dev, uint32_t mcu_va, size_t size)
{
	struct pw_fw_section *sh = dev->fw.shared;
	uint64_t offset;

	if (mcu_va < sh->va)
		return NULL;
	offset = mcu_va - sh->va;
	if (offset > sh->size || size > sh->size - offset)
		return NULL;
	return (uint8_t *)pw_os_mem_kva(sh->mem) + offset;
}

static bool fw_has_glb_state(struct pw_device *dev)
{
	return PW_READ_ONCE(dev->fw.glb.control->version) >= CSF_IFACE_VERSION(4, 1, 0);
}

/* fw.c:801-842 */
static int init_cs_iface(struct pw_device *dev, unsigned int csg_idx, unsigned int cs_idx,
			 uint32_t group_stride, uint32_t stream_stride)
{
	struct pw_fw *fw = &dev->fw;
	struct pw_fw_cs_iface *cs = &fw->cs[csg_idx][cs_idx];
	uint64_t sz = fw->shared->size;
	uint64_t off = CSF_GROUP_CONTROL_OFFSET + (uint64_t)csg_idx * group_stride +
		       CSF_STREAM_CONTROL_OFFSET + (uint64_t)cs_idx * stream_stride;
	uint32_t features, in_va, out_va;

	if (off > sz || sizeof(*cs->control) > sz - off)
		return PW_EINVAL;
	cs->control = (const void *)((uint8_t *)pw_os_mem_kva(fw->shared->mem) + off);
	features = PW_READ_ONCE(cs->control->features);
	in_va = PW_READ_ONCE(cs->control->input_va);
	out_va = PW_READ_ONCE(cs->control->output_va);
	cs->input = iface_fw_to_cpu_addr(dev, in_va, sizeof(*cs->input));
	cs->output = iface_fw_to_cpu_addr(dev, out_va, sizeof(*cs->output));
	if (!cs->input || !cs->output) {
		pw_err(dev, "CSG%u/CS%u: invalid stream interface input/output VA (0x%x/0x%x)",
		       csg_idx, cs_idx, in_va, out_va);
		return PW_EINVAL;
	}

	if (csg_idx || cs_idx) {
		if (features != PW_READ_ONCE(fw->cs[0][0].control->features)) {
			pw_err(dev, "expecting identical CS slots");
			return PW_EINVAL;
		}
	} else {
		dev->csif_info.cs_reg_count = CS_FEATURES_WORK_REGS(features);
		dev->csif_info.unpreserved_cs_reg_count = CSF_UNPRESERVED_REG_COUNT;
	}
	return PW_OK;
}

/* fw.c:844-859 */
static bool compare_csg(const struct pw_fw_csg_control_iface *a,
			const struct pw_fw_csg_control_iface *b)
{
	return PW_READ_ONCE(a->features) == PW_READ_ONCE(b->features) &&
	       PW_READ_ONCE(a->suspend_size) == PW_READ_ONCE(b->suspend_size) &&
	       PW_READ_ONCE(a->protm_suspend_size) == PW_READ_ONCE(b->protm_suspend_size) &&
	       PW_READ_ONCE(a->stream_num) == PW_READ_ONCE(b->stream_num);
}

/* fw.c:861-905 */
static int init_csg_iface(struct pw_device *dev, unsigned int csg_idx, uint32_t group_stride)
{
	struct pw_fw *fw = &dev->fw;
	struct pw_fw_csg_iface *csg = &fw->csg[csg_idx];
	uint64_t sz = fw->shared->size;
	uint64_t off = CSF_GROUP_CONTROL_OFFSET + (uint64_t)csg_idx * group_stride;
	uint32_t stream_num, stream_stride, in_va, out_va;

	if (off > sz || sizeof(*csg->control) > sz - off)
		return PW_EINVAL;
	csg->control = (const void *)((uint8_t *)pw_os_mem_kva(fw->shared->mem) + off);
	in_va = PW_READ_ONCE(csg->control->input_va);
	out_va = PW_READ_ONCE(csg->control->output_va);
	stream_num = PW_READ_ONCE(csg->control->stream_num);
	stream_stride = PW_READ_ONCE(csg->control->stream_stride);
	csg->input = iface_fw_to_cpu_addr(dev, in_va, sizeof(*csg->input));
	csg->output = iface_fw_to_cpu_addr(dev, out_va, sizeof(*csg->output));

	if (stream_num < MIN_CS_PER_CSG || stream_num > MAX_CS_PER_CSG) {
		pw_err(dev, "CSG%u: invalid stream count %u", csg_idx, stream_num);
		return PW_EINVAL;
	}
	if (!csg->input || !csg->output) {
		pw_err(dev, "CSG%u: invalid group interface input/output VA (0x%x/0x%x)", csg_idx,
		       in_va, out_va);
		return PW_EINVAL;
	}
	if (csg_idx > 0 && !compare_csg(fw->csg[0].control, csg->control)) {
		pw_err(dev, "expecting identical CSG slots");
		return PW_EINVAL;
	}
	for (unsigned int i = 0; i < stream_num; i++) {
		int ret = init_cs_iface(dev, csg_idx, i, group_stride, stream_stride);

		if (ret)
			return ret;
	}
	return PW_OK;
}

/* fw.c:1010-1059 (+ the csif_info part of panthor_sched_init, sched.c:4100-4132) */
static int fw_init_ifaces(struct pw_device *dev)
{
	struct pw_fw *fw = &dev->fw;
	struct pw_fw_glb_iface *glb = &fw->glb;
	uint32_t version, in_va, out_va, group_num, group_stride, gpu_as_count, num_groups;
	const struct pw_fw_csg_control_iface *csg0;

	if (!pw_os_mem_kva(fw->shared->mem) || sizeof(*glb->control) > fw->shared->size)
		return PW_EINVAL;
	/* MCU-written data: make sure we don't read stale values (no-op for WC mappings). */
	pw_os_mem_invalidate(dev->os, fw->shared->mem, 0, fw->shared->size);
	pw_rmb();

	glb->control = pw_os_mem_kva(fw->shared->mem);
	version = PW_READ_ONCE(glb->control->version);
	if (!version) {
		pw_err(dev, "firmware interface version is 0: firmware may have failed to boot");
		return PW_EINVAL;
	}
	in_va = PW_READ_ONCE(glb->control->input_va);
	out_va = PW_READ_ONCE(glb->control->output_va);
	glb->input = iface_fw_to_cpu_addr(dev, in_va, sizeof(*glb->input));
	glb->output = iface_fw_to_cpu_addr(dev, out_va, sizeof(*glb->output));
	if (!glb->input || !glb->output) {
		pw_err(dev, "invalid global interface input/output VA (0x%x/0x%x)", in_va, out_va);
		return PW_EINVAL;
	}

	group_num = PW_READ_ONCE(glb->control->group_num);
	group_stride = PW_READ_ONCE(glb->control->group_stride);
	if (group_num > MAX_CSGS || group_num < MIN_CSGS) {
		pw_err(dev, "invalid number of command stream groups (%u)", group_num);
		return PW_EINVAL;
	}
	for (unsigned int i = 0; i < group_num; i++) {
		int ret = init_csg_iface(dev, i, group_stride);

		if (ret)
			return ret;
	}

	/* sched.c:4117-4123: at least one AS besides AS0 (MCU). */
	gpu_as_count = pw_hweight64(dev->gpu_info.as_present & PW_GENMASK(31, 1));
	if (!gpu_as_count) {
		pw_err(dev, "not enough address spaces (%u, expected at least 2)", gpu_as_count + 1);
		return PW_EINVAL;
	}

	/* sched.c:4103-4115 */
	num_groups = PW_MIN((uint32_t)MAX_CSGS, group_num);
	num_groups = PW_MIN((uint32_t)MAX_CSG_PRIO + 1, num_groups);
	csg0 = fw->csg[0].control;
	dev->csif_info.csg_slot_count = num_groups;
	dev->csif_info.cs_slot_count = PW_READ_ONCE(csg0->stream_num);
	dev->csif_info.scoreboard_slot_count =
		CS_FEATURES_SCOREBOARDS(PW_READ_ONCE(fw->cs[0][0].control->features));

	pw_info(dev, "CSF FW using interface v%u.%u.%u, Features %#x Instrumentation features %#x",
		CSF_IFACE_VERSION_MAJOR(version), CSF_IFACE_VERSION_MINOR(version),
		CSF_IFACE_VERSION_PATCH(version), PW_READ_ONCE(glb->control->features),
		version >= CSF_IFACE_VERSION(1, 1, 0) ?
			PW_READ_ONCE(glb->control->instr_features) : 0);
	pw_info(dev, "%u CSG slots x %u CS, %u CS regs, %u scoreboards", group_num,
		dev->csif_info.cs_slot_count, dev->csif_info.cs_reg_count,
		dev->csif_info.scoreboard_slot_count);

	__atomic_store_n(&fw->ifaces_ready, 1, __ATOMIC_RELEASE);
	return PW_OK;
}

/* fw.c:404-445 */
uint32_t pw_fw_conv_timeout(uint32_t timer_hz, uint32_t timeout_us)
{
	bool use_cycle_counter = false;
	uint64_t mod_cycles;

	if (!timer_hz) {
		/* panthor falls back to the GPU core clock with GLB_TIMER_SOURCE_GPU_COUNTER; we
		 * don't know it either, so pick the maximum (fw.c:428-434). */
		return GLB_TIMER_VAL(~0u) | GLB_TIMER_SOURCE_GPU_COUNTER;
	}
	mod_cycles = PW_DIV_ROUND_UP((uint64_t)timeout_us * timer_hz, 1000000ull << 10);
	if (mod_cycles > GLB_TIMER_VAL(~0u))
		mod_cycles = GLB_TIMER_VAL(~0u);
	return GLB_TIMER_VAL((uint32_t)mod_cycles) |
	       (use_cycle_counter ? GLB_TIMER_SOURCE_GPU_COUNTER : 0);
}

void pw_fw_ring_doorbell(struct pw_device *dev, uint32_t id)
{
	pw_wmb();	/* interface writes (WC) before the doorbell (Device) */
	gpu_write(dev, CSF_DOORBELL(id), 1);
}

/* fw.c:1438-1444 */
void pw_fw_ring_csg_doorbells(struct pw_device *dev, uint32_t csg_mask)
{
	struct pw_fw_glb_iface *glb = &dev->fw.glb;

	pw_fw_toggle_reqs(dev, glb, doorbell_req, doorbell_ack, csg_mask);
	pw_fw_ring_doorbell(dev, CSF_GLB_DOORBELL_ID);
}

/* fw.c:1061-1096 */
static void fw_init_global_iface(struct pw_device *dev)
{
	struct pw_fw_glb_iface *glb = &dev->fw.glb;
	uint32_t ack_irq_mask;

	PW_WRITE_ONCE(glb->input->core_en_mask, dev->gpu_info.shader_present);
	PW_WRITE_ONCE(glb->input->poweroff_timer,
		      pw_fw_conv_timeout(dev->fw.timer_hz, PWROFF_HYSTERESIS_US));
	PW_WRITE_ONCE(glb->input->progress_timer,
		      (uint32_t)(PROGRESS_TIMEOUT_CYCLES >> PROGRESS_TIMEOUT_SCALE_SHIFT));
	PW_WRITE_ONCE(glb->input->idle_timer,
		      pw_fw_conv_timeout(dev->fw.timer_hz, IDLE_HYSTERESIS_US));

	ack_irq_mask = GLB_CFG_ALLOC_EN | GLB_PING | GLB_CFG_PROGRESS_TIMER |
		       GLB_CFG_POWEROFF_TIMER | GLB_IDLE_EN | GLB_IDLE;
	if (fw_has_glb_state(dev))
		ack_irq_mask |= GLB_STATE_MASK;
	PW_WRITE_ONCE(glb->input->ack_irq_mask, ack_irq_mask);

	pw_fw_update_reqs(dev, glb, req, GLB_IDLE_EN | GLB_COUNTER_EN,
			  GLB_IDLE_EN | GLB_COUNTER_EN);
	pw_fw_toggle_reqs(dev, glb, req, ack,
			  GLB_CFG_ALLOC_EN | GLB_CFG_POWEROFF_TIMER | GLB_CFG_PROGRESS_TIMER);

	pw_fw_ring_doorbell(dev, CSF_GLB_DOORBELL_ID);
	/* TODO: periodic watchdog ping (fw.c:1446-1466, every 12 s) needs an OS timer. */
}

/*
 * fw.c:1260-1296: busy-wait briefly, then sleep. The sleep is bounded to 1 ms slices and the ack
 * is re-read each time, so this also works when the job IRQ isn't delivered.
 */
static int fw_wait_acks(struct pw_device *dev, const uint32_t *req_ptr, const uint32_t *ack_ptr,
			uint32_t req_mask, uint32_t *acked, uint32_t timeout_ms)
{
	uint32_t req = PW_READ_ONCE(*req_ptr) & req_mask, ack;
	uint64_t now = pw_os_time_ns(dev->os);
	uint64_t spin_end = now + 10000, deadline = now + (uint64_t)timeout_ms * 1000000u;

	*acked = req_mask;
	for (;;) {
		/* req_event is manual-reset on Windows and nothing else resets it after boot:
		 * without this every wait below would return at once and we would spin for the
		 * whole timeout. Reset before re-reading the ack so a signal for this ack isn't
		 * lost; a concurrent waiter can lose at most one 1 ms slice. */
		if (now > spin_end)
			pw_os_event_reset(dev->fw.req_event);
		ack = PW_READ_ONCE(*ack_ptr);
		if ((ack & req_mask) == req)
			return PW_OK;
		now = pw_os_time_ns(dev->os);
		if (now > deadline)
			break;
		if (now > spin_end)
			pw_os_event_wait(dev->fw.req_event, 1);
	}
	ack = PW_READ_ONCE(*ack_ptr);
	if ((ack & req_mask) == req)
		return PW_OK;
	*acked = ~(req ^ ack) & req_mask;
	return PW_ETIMEDOUT;
}

/* fw.c:1298-1324 */
int pw_fw_glb_wait_acks(struct pw_device *dev, uint32_t req_mask, uint32_t *acked,
			uint32_t timeout_ms)
{
	struct pw_fw_glb_iface *glb = &dev->fw.glb;

	/* GLB_HALT isn't acked through the FW interface. */
	if (req_mask & (~GLB_REQ_MASK | GLB_HALT))
		return PW_EINVAL;
	if (!__atomic_load_n(&dev->fw.ifaces_ready, __ATOMIC_ACQUIRE))
		return PW_EINVAL;
	return fw_wait_acks(dev, &glb->input->req, &glb->output->ack, req_mask, acked, timeout_ms);
}

/* fw.c:1326-1360 */
int pw_fw_csg_wait_acks(struct pw_device *dev, uint32_t csg, uint32_t req_mask, uint32_t *acked,
			uint32_t timeout_ms)
{
	struct pw_fw_csg_iface *iface;
	int ret;

	if (csg >= MAX_CSGS || (req_mask & ~CSG_REQ_MASK) ||
	    !__atomic_load_n(&dev->fw.ifaces_ready, __ATOMIC_ACQUIRE))
		return PW_EINVAL;
	iface = &dev->fw.csg[csg];
	if (!iface->input)
		return PW_EINVAL;
	ret = fw_wait_acks(dev, &iface->input->req, &iface->output->ack, req_mask, acked,
			   timeout_ms);
	/* All state bits must have been updated for the state to be meaningful. */
	if ((*acked & CSG_STATE_MASK) != CSG_STATE_MASK)
		*acked &= ~CSG_STATE_MASK;
	return ret;
}

/* fw.c:1446-1466 (one ping, no rescheduling) */
int pw_fw_ping(struct pw_device *dev, uint32_t timeout_ms)
{
	struct pw_fw_glb_iface *glb = &dev->fw.glb;
	uint32_t acked;
	int ret;

	if (!__atomic_load_n(&dev->fw.ifaces_ready, __ATOMIC_ACQUIRE))
		return PW_EINVAL;
	pw_fw_toggle_reqs(dev, glb, req, ack, GLB_PING);
	pw_fw_ring_doorbell(dev, CSF_GLB_DOORBELL_ID);
	ret = pw_fw_glb_wait_acks(dev, GLB_PING, &acked, timeout_ms);
	if (ret)
		pw_err(dev, "FW ping timeout (req 0x%08x ack 0x%08x)", PW_READ_ONCE(glb->input->req),
		       PW_READ_ONCE(glb->output->ack));
	return ret;
}

/* fw.c:1098-1124 */
void pw_fw_job_irq_handler(struct pw_device *dev, uint32_t status)
{
	struct pw_fw *fw = &dev->fw;

	gpu_write(dev, JOB_INT_BASE + INT_CLEAR, status);

	if (!__atomic_load_n(&fw->booted, __ATOMIC_ACQUIRE) && (status & JOB_INT_GLOBAL_IF))
		__atomic_store_n(&fw->booted, 1, __ATOMIC_RELEASE);
	pw_os_event_signal(fw->req_event);

	if (!__atomic_load_n(&fw->booted, __ATOMIC_ACQUIRE))
		return;

	if (status & JOB_INT_GLOBAL_IF) {
		/* sched.c:1848-1876: acknowledge GLB_IDLE events. */
		struct pw_fw_glb_iface *glb = &fw->glb;
		uint32_t req, ack, evts = 0;

		pw_os_mutex_lock(fw->req_lock);
		if (__atomic_load_n(&fw->ifaces_ready, __ATOMIC_ACQUIRE)) {
			pw_rmb();
			req = PW_READ_ONCE(glb->input->req);
			ack = PW_READ_ONCE(glb->output->ack);
			evts = (req ^ ack) & GLB_EVT_MASK;
			if (evts & GLB_IDLE)
				PW_WRITE_ONCE(glb->input->req, pw_fw_update_val(req, ack, GLB_IDLE));
		}
		pw_os_mutex_unlock(fw->req_lock);
		if (evts)
			pw_dbg(dev, "GLB events 0x%08x", evts);
	}
	if (status & ~JOB_INT_GLOBAL_IF)
		pw_dbg(dev, "CSG IRQ 0x%08x ignored (no scheduler yet)", status & ~JOB_INT_GLOBAL_IF);
}

/* fw.c:1126-1165 */
static int fw_start(struct pw_device *dev)
{
	struct pw_fw *fw = &dev->fw;
	uint64_t deadline;

	__atomic_store_n(&fw->booted, 0, __ATOMIC_RELEASE);
	pw_os_event_reset(fw->req_event);
	pw_irq_enable_events(dev, PW_IRQ_JOB, ~0u);
	pw_irq_resume(dev, PW_IRQ_JOB);
	gpu_write(dev, MCU_CONTROL, MCU_CONTROL_AUTO);

	deadline = pw_os_time_ns(dev->os) + (uint64_t)PW_FW_BOOT_TIMEOUT_MS * 1000000u;
	for (;;) {
		bool expired = pw_os_time_ns(dev->os) > deadline;

		if (__atomic_load_n(&fw->booted, __ATOMIC_ACQUIRE))
			return PW_OK;
		/* Deviation: RAWSTAT instead of INT_STAT, and polled every slice, so a boot is
		 * detected even before the OS layer delivers interrupts. */
		if (gpu_read(dev, JOB_INT_BASE + INT_RAWSTAT) & JOB_INT_GLOBAL_IF) {
			__atomic_store_n(&fw->booted, 1, __ATOMIC_RELEASE);
			return PW_OK;
		}
		if (expired)
			break;
		pw_os_event_wait(fw->req_event, 10);
	}

	{
		static const char *const status_str[] = {
			[MCU_STATUS_DISABLED] = "disabled",
			[MCU_STATUS_ENABLED] = "enabled",
			[MCU_STATUS_HALT] = "halt",
			[MCU_STATUS_FATAL] = "fatal",
		};
		uint32_t st = gpu_read(dev, MCU_STATUS);

		pw_err(dev, "Failed to boot MCU (status=%s, MCU_STATUS 0x%x, JOB_INT_RAWSTAT 0x%08x, "
		       "GPU_FAULT_STATUS 0x%08x)", st < PW_ARRAY_SIZE(status_str) ? status_str[st] :
		       "unknown", st, gpu_read(dev, JOB_INT_BASE + INT_RAWSTAT),
		       gpu_read(dev, GPU_FAULT_STATUS));
	}
	return PW_ETIMEDOUT;
}

/* fw.c:1167-1176 */
static void fw_stop(struct pw_device *dev)
{
	uint32_t st;

	gpu_write(dev, MCU_CONTROL, MCU_CONTROL_DISABLE);
	if (pw_poll_reg32(dev, MCU_STATUS, ~0u, MCU_STATUS_DISABLED, 10, 100000, &st))
		pw_err(dev, "Failed to stop MCU (MCU_STATUS %u)", st);
}

/* fw.c:1495-1556 (panthor_fw_init), L2 power-on is done by the caller beforehand. */
int pw_fw_init(struct pw_device *dev)
{
	struct pw_fw *fw = &dev->fw;
	uint32_t acked;
	int ret;

	fw->timer_hz = PW_DEFAULT_TIMER_HZ;
	if (!fw->req_event)
		fw->req_event = pw_os_event_create(dev->os);
	if (!fw->req_lock)
		fw->req_lock = pw_os_mutex_create(dev->os);
	if (!fw->req_event || !fw->req_lock)
		return PW_ENOMEM;
	fw->started = true;

	ret = pw_irq_init(dev, PW_IRQ_JOB, JOB_INT_BASE, pw_fw_job_irq_handler);
	if (ret)
		return ret;

	ret = pw_vm_create(dev, true, &fw->vm);
	if (ret)
		return ret;

	ret = fw_load(dev);
	if (ret)
		return ret;

	ret = pw_vm_activate(fw->vm);
	if (ret)
		return ret;

	ret = fw_start(dev);
	if (ret)
		return ret;

	ret = fw_init_ifaces(dev);
	if (ret)
		return ret;

	fw_init_global_iface(dev);

	/* Not in panthor's init path: one synchronous GLB_PING proves the MCU processes
	 * requests (milestone M2's exit criterion). */
	ret = pw_fw_ping(dev, PW_FW_PING_TIMEOUT_MS);
	if (ret)
		return ret;
	if (pw_fw_glb_wait_acks(dev, GLB_CFG_ALLOC_EN | GLB_CFG_POWEROFF_TIMER |
				GLB_CFG_PROGRESS_TIMER, &acked, 1))
		pw_warn(dev, "FW has not acked the CFG requests yet (acked 0x%x)", acked);
	return PW_OK;
}

/*
 * fw.c:1219-1258 (panthor_fw_unplug). Quiesces the MCU and frees its memory, but keeps the
 * mutex/event alive: a job bottom half may still be running (pw_fw_release() frees them).
 */
void pw_fw_fini(struct pw_device *dev)
{
	struct pw_fw *fw = &dev->fw;

	if (!fw->started)
		return;
	fw->started = false;

	pw_irq_suspend(dev, PW_IRQ_JOB);
	fw_stop(dev);

	/* The job bottom half only touches the interfaces under req_lock with ifaces_ready set. */
	pw_os_mutex_lock(fw->req_lock);
	__atomic_store_n(&fw->ifaces_ready, 0, __ATOMIC_RELEASE);
	pw_memset(&fw->glb, 0, sizeof(fw->glb));
	pw_memset(fw->csg, 0, sizeof(fw->csg));
	pw_memset(fw->cs, 0, sizeof(fw->cs));
	pw_os_mutex_unlock(fw->req_lock);

	/* Tear down the MCU VM (AS0 disable, cache flush) before freeing what it maps. */
	pw_vm_destroy(fw->vm);
	fw->vm = NULL;
	fw_free_sections(dev);
	__atomic_store_n(&fw->booted, 0, __ATOMIC_RELEASE);
	pw_os_event_signal(fw->req_event);
}

void pw_fw_release(struct pw_device *dev)
{
	struct pw_fw *fw = &dev->fw;

	if (fw->req_lock)
		pw_os_mutex_destroy(dev->os, fw->req_lock);
	if (fw->req_event)
		pw_os_event_destroy(dev->os, fw->req_event);
	fw->req_lock = NULL;
	fw->req_event = NULL;
}
