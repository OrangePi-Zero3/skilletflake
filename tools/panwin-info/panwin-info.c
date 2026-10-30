/* SPDX-License-Identifier: MIT */
/*
 * panwin-info: bring-up tool for panwin.sys. Opens a device through libdrm-win and prints
 * PANWIN_IOCTL_GET_INFO, drmGetVersion, DEV_QUERY GPU_INFO / CSIF_INFO (decoded), and optional
 * raw register reads (PANWIN_IOCTL_DEBUG_REG_READ, debug builds of panwin.sys only).
 *
 *   panwin-info [--device N] [--reg OFFSET]... [--fake] [--list]
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xf86drm.h>
#include <panthor_drm.h>
#include <panwin/panwin_ioctl.h>
#include <libdrm_win.h>

#define MAX_REGS 32

/* UCRT's strerror() has no text for the POSIX-supplement errnos (ETIME, EOPNOTSUPP, ...). */
static const char *err_str(int e)
{
	static char buf[96];
	snprintf(buf, sizeof(buf), "%s (errno %d)", strerror(e), e);
	return buf;
}

static const char *product_name(uint32_t gpu_id)
{
	/* (arch_major, arch_minor, product_major) -> name, from Mesa's pan_model.c (MIT). */
	static const struct { unsigned maj, min, prod; const char *name; } t[] = {
		{ 9, 0, 1, "Mali-G57" }, { 9, 0, 3, "Mali-G57" }, { 9, 2, 4, "Mali-G68" },
		{ 10, 8, 7, "Mali-G610" }, { 10, 12, 4, "Mali-G310" }, { 11, 8, 2, "Mali-G715" },
		{ 11, 8, 3, "Mali-G615" }, { 12, 8, 0, "Mali-G720" }, { 13, 8, 0, "Mali-G725" },
	};
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (DRM_PANTHOR_ARCH_MAJOR(gpu_id) == t[i].maj && DRM_PANTHOR_ARCH_MINOR(gpu_id) == t[i].min &&
		    DRM_PANTHOR_PRODUCT_MAJOR(gpu_id) == t[i].prod)
			return t[i].name;
	return "unknown";
}

static int popcount64(uint64_t v)
{
	int n = 0;
	for (; v; v &= v - 1)
		n++;
	return n;
}

static const char *coherency_name(uint32_t c)
{
	switch (c) {
	case DRM_PANTHOR_GPU_COHERENCY_ACE_LITE: return "ACE-Lite";
	case DRM_PANTHOR_GPU_COHERENCY_ACE: return "ACE";
	case DRM_PANTHOR_GPU_COHERENCY_NONE: return "none";
	default: return "?";
	}
}

static int dev_query(int fd, uint32_t type, void *data, uint32_t size)
{
	struct drm_panthor_dev_query q = { .type = type, .size = size, .pointer = (uint64_t)(uintptr_t)data };
	return drmIoctl(fd, DRM_IOCTL_PANTHOR_DEV_QUERY, &q);
}

static void print_gpu_info(const struct drm_panthor_gpu_info *g)
{
	uint32_t id = g->gpu_id;

	printf("GPU_INFO:\n");
	printf("  gpu_id            0x%08" PRIx32 "  %s r%up%u (arch %u.%u.%u, product major %u, status %u)\n",
	       id, product_name(id), DRM_PANTHOR_VERSION_MAJOR(id), DRM_PANTHOR_VERSION_MINOR(id),
	       DRM_PANTHOR_ARCH_MAJOR(id), DRM_PANTHOR_ARCH_MINOR(id), DRM_PANTHOR_ARCH_REV(id),
	       DRM_PANTHOR_PRODUCT_MAJOR(id), DRM_PANTHOR_VERSION_STATUS(id));
	printf("  gpu_rev           0x%08" PRIx32 "\n", g->gpu_rev);
	printf("  csf_id            0x%08" PRIx32 "  CSHW %u.%u.%u, MCU %u.%u.%u\n", g->csf_id,
	       DRM_PANTHOR_CSHW_MAJOR(g->csf_id), DRM_PANTHOR_CSHW_MINOR(g->csf_id),
	       DRM_PANTHOR_CSHW_REV(g->csf_id), DRM_PANTHOR_MCU_MAJOR(g->csf_id),
	       DRM_PANTHOR_MCU_MINOR(g->csf_id), DRM_PANTHOR_MCU_REV(g->csf_id));
	printf("  l2_features       0x%08" PRIx32 "  line %u B, L2 slice 2^%u B\n", g->l2_features,
	       1u << (g->l2_features & 0xff), (g->l2_features >> 16) & 0xff);
	printf("  tiler_features    0x%08" PRIx32 "\n", g->tiler_features);
	printf("  mem_features      0x%08" PRIx32 "\n", g->mem_features);
	printf("  mmu_features      0x%08" PRIx32 "  VA %u bits, PA %u bits\n", g->mmu_features,
	       DRM_PANTHOR_MMU_VA_BITS(g->mmu_features), (g->mmu_features >> 8) & 0xff);
	printf("  thread_features   0x%08" PRIx32 "  max_threads %" PRIu32 ", max_workgroup %" PRIu32
	       ", max_barrier %" PRIu32 "\n", g->thread_features, g->max_threads,
	       g->thread_max_workgroup_size, g->thread_max_barrier_size);
	printf("  coherency         features 0x%" PRIx32 ", selected %s (%" PRIu32 ")\n",
	       g->coherency_features, coherency_name(g->selected_coherency), g->selected_coherency);
	printf("  texture_features  0x%08" PRIx32 " 0x%08" PRIx32 " 0x%08" PRIx32 " 0x%08" PRIx32 "\n",
	       g->texture_features[0], g->texture_features[1], g->texture_features[2], g->texture_features[3]);
	printf("  as_present        0x%08" PRIx32 "  (%d address spaces)\n", g->as_present,
	       popcount64(g->as_present));
	printf("  shader_present    0x%016" PRIx64 "  (%d cores)\n", g->shader_present,
	       popcount64(g->shader_present));
	printf("  tiler_present     0x%016" PRIx64 "\n", g->tiler_present);
	printf("  l2_present        0x%016" PRIx64 "  (%d slices)\n", g->l2_present, popcount64(g->l2_present));
	printf("  core_features     0x%08" PRIx32 "\n", g->core_features);
	printf("  gpu_features      0x%016" PRIx64 "\n", g->gpu_features);
}

static void usage(const char *argv0)
{
	printf("usage: %s [--device N] [--reg OFFSET]... [--fake] [--list]\n"
	       "  --device N    open /dev/dri/renderD(128+N) (default 0)\n"
	       "  --reg OFFSET  read a GPU register (hex or decimal; debug panwin.sys builds only)\n"
	       "  --fake        use libdrm-win's in-process fake kernel (needs -Dfake_kernel=true)\n"
	       "  --list        only list devices\n", argv0);
}

int main(int argc, char **argv)
{
	int index = 0, nregs = 0, list_only = 0, rc = 0;
	uint32_t regs[MAX_REGS];

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			index = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--reg") && i + 1 < argc && nregs < MAX_REGS) {
			regs[nregs++] = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--fake")) {
			if (panwin_use_fake_kernel(1)) {
				fprintf(stderr, "--fake: %s\n", err_str(errno));
				return 2;
			}
		} else if (!strcmp(argv[i], "--list")) {
			list_only = 1;
		} else {
			usage(argv[0]);
			return !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") ? 0 : 2;
		}
	}

	printf("libdrm-win transport: %s\n", panwin_transport_name());

	drmDevicePtr devs[16];
	int n = drmGetDevices2(0, devs, 16);
	if (n < 0) {
		fprintf(stderr, "drmGetDevices2: %s\n", err_str(-n));
		return 1;
	}
	printf("devices: %d\n", n);
	for (int i = 0; i < n; i++) {
		char path[512] = "?";
		panwin_device_path(i, path, sizeof(path));
		printf("  [%d] %s  bus=platform %s  win32=%s\n", i, devs[i]->nodes[DRM_NODE_RENDER],
		       devs[i]->businfo.platform->fullname, path);
	}
	drmFreeDevices(devs, n);
	if (list_only)
		return 0;
	if (index >= n) {
		fprintf(stderr, "no device %d (is panwin.sys installed and started?)\n", index);
		return 1;
	}

	char node[32];
	snprintf(node, sizeof(node), "/dev/dri/renderD%d", 128 + index);
	int fd = panwin_open(node, 0);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", node, err_str(errno));
		return 1;
	}

	struct panwin_info info;
	memset(&info, 0, sizeof(info));
	if (panwin_private_ioctl(fd, PANWIN_IOCTL_GET_INFO, &info, sizeof(info))) {
		printf("GET_INFO: failed: %s\n", err_str(errno));
		rc = 1;
	} else {
		info.description[sizeof(info.description) - 1] = '\0';
		printf("GET_INFO:\n  abi_version %u%s, flags 0x%x%s%s, emulated DRM %u.%u, MMIO base 0x%" PRIx64 "\n"
		       "  \"%s\"\n", info.abi_version,
		       info.abi_version == PANWIN_ABI_VERSION ? "" : " (MISMATCH with this build!)", info.flags,
		       (info.flags & PANWIN_INFO_FW_BOOTED) ? " fw-booted" : "",
		       (info.flags & PANWIN_INFO_DEBUG_BUILD) ? " debug-build" : "", info.drm_version_major,
		       info.drm_version_minor, info.gpu_phys_base, info.description);
	}

	drmVersionPtr v = drmGetVersion(fd);
	if (!v) {
		printf("drmGetVersion: failed: %s\n", err_str(errno));
		rc = 1;
	} else {
		printf("drmGetVersion: %s %d.%d.%d (%s) \"%s\"\n", v->name, v->version_major,
		       v->version_minor, v->version_patchlevel, v->date, v->desc);
		drmFreeVersion(v);
	}

	uint64_t cap = 0;
	if (drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) == 0)
		printf("DRM_CAP_SYNCOBJ_TIMELINE: %" PRIu64 "%s\n", cap, cap ? "" : " (panvk on CSF needs 1)");
	else
		printf("DRM_CAP_SYNCOBJ_TIMELINE: failed: %s\n", err_str(errno));

	struct drm_panthor_gpu_info gpu;
	memset(&gpu, 0, sizeof(gpu));
	if (dev_query(fd, DRM_PANTHOR_DEV_QUERY_GPU_INFO, &gpu, sizeof(gpu))) {
		printf("GPU_INFO: failed: %s\n", err_str(errno));
		rc = 1;
	} else {
		print_gpu_info(&gpu);
	}

	struct drm_panthor_csif_info csif;
	memset(&csif, 0, sizeof(csif));
	if (dev_query(fd, DRM_PANTHOR_DEV_QUERY_CSIF_INFO, &csif, sizeof(csif))) {
		printf("CSIF_INFO: failed: %s\n", err_str(errno));
		rc = 1;
	} else {
		printf("CSIF_INFO:\n  csg_slots %u, cs_slots/group %u, cs_regs %u, scoreboards %u, "
		       "unpreserved_cs_regs %u\n", csif.csg_slot_count, csif.cs_slot_count, csif.cs_reg_count,
		       csif.scoreboard_slot_count, csif.unpreserved_cs_reg_count);
	}

	for (int i = 0; i < nregs; i++) {
		struct panwin_debug_reg_read rr = { .offset = regs[i] };
		if (panwin_private_ioctl(fd, PANWIN_IOCTL_DEBUG_REG_READ, &rr, sizeof(rr))) {
			printf("reg 0x%05" PRIx32 ": failed: %s\n", regs[i], err_str(errno));
			rc = 1;
		} else {
			printf("reg 0x%05" PRIx32 " = 0x%08" PRIx32 "\n", regs[i], rr.value);
		}
	}

	panwin_close(fd);
	return rc;
}
