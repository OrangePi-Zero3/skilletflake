// SPDX-License-Identifier: MIT
/*
 * test_device.c - full bring-up against the mock (GPU -> MMU -> FW boot -> ifaces -> ping),
 * failure paths, and the IRQ top/bottom-half paths including GPU and MMU fault decoding.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mock_gpu.h"
#include "pw_mmu.h"
#include "test.h"

static struct pw_os *new_os(enum mock_fw_variant v)
{
	struct mock_config cfg = { 0 };

	cfg.fw_path = g_fw_path;
	cfg.fw_variant = v;
	return mock_create(&cfg);
}

struct mask_wait {
	struct pw_os *os;
	unsigned int line;
	uint32_t want;
};

static bool mask_is(void *arg)
{
	struct mask_wait *w = arg;

	return mock_int_mask(w->os, w->line) == w->want;
}

static bool wait_mask(struct pw_os *os, unsigned int line, uint32_t want)
{
	struct mask_wait w = { os, line, want };

	if (wait_until(mask_is, &w, 1000))
		return true;
	fprintf(stderr, "    line %u INT_MASK 0x%x, want 0x%x\n", line, mock_int_mask(os, line), want);
	return false;
}

/* alloc + attach IRQs + probe: the order the Windows glue should use. */
static int bringup(struct pw_os *os, bool boot_fw, struct pw_device **out)
{
	struct pw_device_params p = { .os = os, .fw_name = NULL, .boot_firmware = boot_fw };
	int ret;

	ret = pw_device_alloc(&p, out);
	if (ret)
		return ret;
	mock_attach_irq(os, *out);
	return pw_device_probe(*out);
}

static void teardown(struct pw_os *os, struct pw_device *dev)
{
	mock_detach_irq(os);
	pw_device_destroy(dev);
	check_no_leaks(os);
	mock_destroy(os);
}

static void test_bringup(void)
{
	struct pw_os *os;
	struct pw_device *dev;
	struct drm_panthor_gpu_info gi;
	struct drm_panthor_csif_info ci;
	struct drm_panthor_mmu_info mi;
	struct mock_as_state as0;
	struct mock_glb_snapshot glb;
	char desc[160];
	uint8_t *img;
	size_t img_size;
	uint64_t t0;

	if (!g_fw_path) {
		test_skip("real firmware not available");
		return;
	}
	os = new_os(MOCK_FW_OK);
	t0 = now_ms();
	REQUIRE(bringup(os, true, &dev) == PW_OK);
	fprintf(stderr, "    bring-up took %llu ms\n", (unsigned long long)(now_ms() - t0));

	CHECK(!strcmp(mock_last_fw_name(os), "arm/mali/arch10.8/mali_csffw.bin"));
	CHECK(pw_device_fw_booted(dev));
	CHECK_EQ(mock_soft_resets(os), 1);
	CHECK_EQ(mock_model_errors(os), 0);

	/* GPU info exactly as panthor fills it */
	pw_device_get_gpu_info(dev, &gi);
	CHECK_EQ(gi.gpu_id, MOCK_G610_GPU_ID);
	CHECK_EQ(gi.csf_id, MOCK_G610_CSF_ID);
	CHECK_EQ(gi.l2_features, MOCK_G610_L2_FEATURES);
	CHECK_EQ(gi.tiler_features, MOCK_G610_TILER_FEATURES);
	CHECK_EQ(gi.mem_features, MOCK_G610_MEM_FEATURES);
	CHECK_EQ(gi.mmu_features, MOCK_G610_MMU_FEATURES);
	CHECK_EQ(gi.thread_features, MOCK_G610_THREAD_FEATURES);
	CHECK_EQ(gi.max_threads, MOCK_G610_MAX_THREADS);
	CHECK_EQ(gi.thread_max_workgroup_size, MOCK_G610_MAX_WG_SIZE);
	CHECK_EQ(gi.thread_max_barrier_size, MOCK_G610_MAX_BARRIER);
	CHECK_EQ(gi.texture_features[0], MOCK_G610_TEXTURE_FEATURES0);
	CHECK_EQ(gi.as_present, MOCK_G610_AS_PRESENT);
	CHECK_EQ(gi.shader_present, MOCK_G610_SHADER_PRESENT);
	CHECK_EQ(gi.l2_present, MOCK_G610_L2_PRESENT);
	CHECK_EQ(gi.tiler_present, MOCK_G610_TILER_PRESENT);
	CHECK_EQ(gi.selected_coherency, DRM_PANTHOR_GPU_COHERENCY_NONE);
	CHECK_EQ(gi.pad, 0);
	CHECK_EQ(mock_reg(os, 0x304), 31);	/* GPU_COHERENCY_PROTOCOL = NONE */

	CHECK_EQ(pw_device_get_csif_info(dev, &ci), PW_OK);
	CHECK_EQ(ci.csg_slot_count, 8);
	CHECK_EQ(ci.cs_slot_count, 8);
	CHECK_EQ(ci.cs_reg_count, 96);
	CHECK_EQ(ci.scoreboard_slot_count, 8);
	CHECK_EQ(ci.unpreserved_cs_reg_count, 4);
	pw_device_get_mmu_info(dev, &mi);
	CHECK_EQ(mi.page_size_bitmap, 0x201000);

	/* MCU VM on AS0 */
	mock_get_as(os, 0, &as0);
	CHECK(as0.enabled);
	CHECK_EQ(as0.transtab, pw_vm_root_pa(dev->fw.vm));
	CHECK_EQ(as0.transcfg, 0x420001c6ull);
	CHECK_EQ(as0.memattr, 0x4c4c4c4c8c4c8f4cull);

	/* Interrupt masks: GPU = FAULT|PROTM_FAULT|RESET|CLEAN_CACHES, MMU faults only, JOB all */
	/* (a line reads 0 while its bottom half runs, so wait for it to settle) */
	CHECK(wait_mask(os, PW_IRQ_GPU, 0x20103));
	CHECK(wait_mask(os, PW_IRQ_JOB, 0xffffffffu));
	CHECK(wait_mask(os, PW_IRQ_MMU, 0xffff));	/* faults only, never REQ_COMPLETE */

	/* Global interface as programmed by fw_init_global_iface (fw.c:1061) */
	mock_glb_snapshot(os, &glb);
	CHECK(glb.valid);
	CHECK_EQ(glb.core_en_mask, MOCK_G610_SHADER_PRESENT);
	CHECK_EQ(glb.poweroff_timer, 235);	/* 10 ms @ 24 MHz >> 10 */
	CHECK_EQ(glb.idle_timer, 19);		/* 800 us */
	CHECK_EQ(glb.progress_timer, 2560000);
	CHECK_EQ(glb.ack_irq_mask, 0x0400050eu);
	CHECK(glb.req & (1u << 10));		/* IDLE_EN */
	CHECK(glb.req & (1u << 7));		/* COUNTER_EN */
	CHECK(glb.doorbells >= 2);		/* init + ping */
	/* every request acked */
	CHECK_EQ((mock_mcu_glb_req(os) ^ mock_mcu_glb_ack(os)) & 0x7fe, 0);

	/* ping round trips */
	for (int i = 0; i < 5; i++)
		CHECK_EQ(pw_device_fw_ping(dev, 100), PW_OK);

	pw_device_describe(dev, desc, sizeof(desc));
	fprintf(stderr, "    describe: %s\n", desc);
	CHECK(strstr(desc, "Mali-G610 r0p0 arch10.8 (id 0xa867), 4 cores") != NULL);
	CHECK(strstr(desc, "fw booted (iface v1.1.0), 8 CSGs") != NULL);

	/* FW sections, as the GPU sees them through AS0 */
	img = read_file(g_fw_path, &img_size);
	REQUIRE(img);
	{
		uint8_t buf[256];
		uint64_t pte;

		CHECK_EQ(mock_gpu_read(os, 0, 0x800000, buf, sizeof(buf)), 0);
		CHECK(!memcmp(buf, img + 0x26c4, sizeof(buf)));
		CHECK_EQ(mock_gpu_read(os, 0, 0x0, buf, 0x7c), 0);
		CHECK(!memcmp(buf, img + 0x131c, 0x7c));
		/* tail of a ZERO section is zero */
		CHECK_EQ(mock_gpu_read(os, 0, 0x2040000 - 64, buf, 64), 0);
		for (int i = 0; i < 64; i++)
			CHECK_EQ(buf[i], 0);
		/* permissions from the section flags */
		CHECK_EQ(mock_gpu_translate(os, 0, 0x800000, 'x', NULL, NULL, NULL), 0);
		CHECK(mock_gpu_translate(os, 0, 0x800000, 'w', NULL, NULL, NULL) != 0);
		CHECK(mock_gpu_translate(os, 0, 0x400000, 'x', NULL, NULL, NULL) != 0);
		CHECK(mock_gpu_translate(os, 0, 0x400000, 'w', NULL, NULL, NULL) != 0);
		CHECK_EQ(mock_gpu_translate(os, 0, 0x2000000, 'w', NULL, NULL, NULL), 0);
		/* cache attributes: CACHED sections WB, the shared (CACHED_COHERENT) one NC */
		CHECK_EQ(mock_gpu_translate(os, 0, 0x800000, 'r', NULL, NULL, &pte), 0);
		CHECK_EQ(pte & PW_PTE_ATTRINDX_MASK, PW_PTE_ATTRINDX(PW_MAIR_IDX_WB));
		CHECK_EQ(mock_gpu_translate(os, 0, 0x4000000, 'r', NULL, NULL, &pte), 0);
		CHECK_EQ(pte & PW_PTE_ATTRINDX_MASK, PW_PTE_ATTRINDX(PW_MAIR_IDX_NC));
		CHECK(pte & PW_PTE_XN);
		/* protected-mode section is not mapped */
		CHECK(mock_gpu_translate(os, 0, 0x3000000, 'r', NULL, NULL, NULL) != 0);
	}
	free(img);

	CHECK(mock_irq_delivered(os, PW_IRQ_JOB) >= 1);	/* GLOBAL_IF (ping acks) */
	CHECK_EQ(mock_irq_unclaimed(os), 0);
	CHECK_EQ(mock_model_errors(os), 0);

	teardown(os, dev);
}

/* Interrupts never delivered (e.g. probe before the ISR is connected, or wrong ACPI IRQs):
 * every wait falls back to RAWSTAT/ack polling. */
static void test_bringup_no_irq(void)
{
	struct pw_device_params p = { 0 };
	struct pw_device *dev = NULL;
	struct pw_os *os;
	uint64_t t0;

	if (!g_fw_path) {
		test_skip("real firmware not available");
		return;
	}
	os = new_os(MOCK_FW_OK);
	p.os = os;
	p.boot_firmware = true;
	t0 = now_ms();
	CHECK_EQ(pw_device_create(&p, &dev), PW_OK);
	CHECK(now_ms() - t0 < 1000);
	CHECK(pw_device_fw_booted(dev));
	CHECK_EQ(pw_device_fw_ping(dev, 100), PW_OK);
	CHECK_EQ(mock_irq_delivered(os, 0) + mock_irq_delivered(os, 1) + mock_irq_delivered(os, 2), 0);
	/* Connecting the IRQ later drains the pending GLOBAL_IF etc. without trouble. */
	mock_attach_irq(os, dev);
	CHECK_EQ(pw_device_fw_ping(dev, 100), PW_OK);
	CHECK_EQ(mock_model_errors(os), 0);
	teardown(os, dev);
}

/* Completions only visible to the bottom half: reset, flushes and boot must go through IRQs. */
static void test_bringup_irq_completions(void)
{
	struct mock_config cfg = { 0 };
	struct pw_device *dev;
	struct pw_vm *vm;
	struct pw_os *os;
	uint32_t flushes;

	if (!g_fw_path) {
		test_skip("real firmware not available");
		return;
	}
	cfg.fw_path = g_fw_path;
	cfg.hide_completions_from_waiters = true;
	os = mock_create(&cfg);
	REQUIRE(bringup(os, true, &dev) == PW_OK);
	CHECK(mock_irq_delivered(os, PW_IRQ_GPU) >= 1);	/* at least RESET_COMPLETED */
	REQUIRE(pw_vm_create(dev, false, &vm) == PW_OK);
	REQUIRE(pw_vm_activate(vm) == PW_OK);
	flushes = mock_flush_count(os);
	for (int i = 0; i < 20; i++) {
		CHECK_EQ(pw_vm_map_pa(vm, 0x100000 + i * 0x1000, MOCK_PA_BASE, 0x1000, 0), PW_OK);
		CHECK_EQ(pw_gpu_flush_caches(dev, CACHE_CLEAN, CACHE_CLEAN, 0), PW_OK);
	}
	CHECK_EQ(mock_flush_count(os), flushes + 40);
	CHECK(mock_irq_delivered(os, PW_IRQ_GPU) >= 40);
	CHECK(!mock_log_contains(os, "Flush caches timeout"));
	pw_vm_destroy(vm);
	CHECK_EQ(mock_model_errors(os), 0);
	CHECK_EQ(mock_irq_unclaimed(os), 0);
	teardown(os, dev);
}

static void test_m1_mode(void)
{
	struct pw_os *os = new_os(MOCK_FW_OK);
	struct pw_device *dev;
	struct drm_panthor_csif_info ci;
	char desc[160];

	REQUIRE(bringup(os, false, &dev) == PW_OK);
	CHECK(!pw_device_fw_booted(dev));
	CHECK_EQ(pw_device_get_csif_info(dev, &ci), PW_ENOENT);
	CHECK_EQ(pw_device_fw_ping(dev, 10), PW_EINVAL);
	CHECK_EQ(mock_mcu_status(os), 0);
	pw_device_describe(dev, desc, sizeof(desc));
	CHECK(strstr(desc, "M1 mode") != NULL);
	CHECK(dev->gpu.l2_on);
	CHECK(mock_log_contains(os, "Mali-G610 id 0xa867 major 0x0 minor 0x0 status 0x0"));
	CHECK(mock_log_contains(os, "Features: L2:0x7120306 Tiler:0x809 Mem:0x301 MMU:0x2830 AS:0xff"));
	CHECK(mock_log_contains(os, "shader_present=0x50005 l2_present=0x1 tiler_present=0x1"));
	teardown(os, dev);
}

struct fail_case {
	const char *name;
	struct mock_config cfg;
	bool boot_fw;
	int expect;
	const char *log;
};

static void run_fail_case(const struct fail_case *fc)
{
	struct mock_config cfg = fc->cfg;
	struct pw_device *dev = NULL;
	struct pw_os *os;
	int ret;

	if (!cfg.fw_path && !cfg.fw_data && fc->expect != PW_ENOENT)
		cfg.fw_path = g_fw_path;
	os = mock_create(&cfg);
	ret = bringup(os, fc->boot_fw, &dev);
	if (ret != fc->expect) {
		fprintf(stderr, "    case '%s': got %d, want %d\n", fc->name, ret, fc->expect);
		CHECK(!"unexpected probe result");
	}
	if (fc->log && !mock_log_contains(os, fc->log)) {
		fprintf(stderr, "    case '%s': log lacks '%s'\n", fc->name, fc->log);
		CHECK(!"missing log line");
	}
	CHECK(!pw_device_fw_booted(dev));
	teardown(os, dev);
}

static void test_failures(void)
{
	const struct fail_case cases[] = {
		{ "GPU_ID 0", { .gpu_id_zero = true }, true, PW_EIO, "GPU_ID reads 0x00000000" },
		{ "arch 9 (Bifrost)", { .gpu_id = 0x92220000 }, true, PW_EOPNOTSUPP, "unsupported GPU arch 9" },
		{ "arch 14 (needs PWR block)", { .gpu_id = 0xe8000000 }, true, PW_EOPNOTSUPP, "arch 14" },
		{ "reset never completes", { .no_reset_complete = true }, false, PW_ETIMEDOUT,
		  "Soft reset timeout" },
		{ "L2 never ready", { .no_l2_ready = true }, false, PW_ETIMEDOUT, "L2:1 readiness" },
		{ "no firmware file", { 0 }, true, PW_ENOENT, "failed to load firmware image" },
		{ "MCU never boots", { .fw_variant = MOCK_FW_NEVER_BOOTS }, true, PW_ETIMEDOUT,
		  "Failed to boot MCU (status=enabled" },
		{ "iface version 0", { .fw_variant = MOCK_FW_VERSION_ZERO }, true, PW_EINVAL,
		  "interface version is 0" },
		{ "group_num 2", { .fw_variant = MOCK_FW_BAD_GROUP_NUM }, true, PW_EINVAL,
		  "invalid number of command stream groups (2)" },
		{ "glb input VA outside shared", { .fw_variant = MOCK_FW_BAD_INPUT_VA }, true, PW_EINVAL,
		  "invalid global interface input/output VA" },
		{ "stream_num 40", { .fw_variant = MOCK_FW_BAD_STREAM_NUM }, true, PW_EINVAL,
		  "invalid stream count 40" },
		{ "CSG 3 differs", { .fw_variant = MOCK_FW_MISMATCHED_CSG }, true, PW_EINVAL,
		  "expecting identical CSG slots" },
		{ "FW ignores doorbell", { .fw_variant = MOCK_FW_IGNORE_DOORBELL }, true, PW_ETIMEDOUT,
		  "FW ping timeout" },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (!g_fw_path && cases[i].boot_fw && cases[i].expect != PW_ENOENT &&
		    cases[i].expect != PW_EIO && cases[i].expect != PW_EOPNOTSUPP)
			continue;
		fprintf(stderr, "    - %s\n", cases[i].name);
		run_fail_case(&cases[i]);
	}
}

static struct pw_os *g_wait_os;
static const char *g_wait_str;

static bool log_has(void *arg)
{
	(void)arg;
	return mock_log_contains(g_wait_os, g_wait_str);
}

static bool wait_log(struct pw_os *os, const char *s)
{
	g_wait_os = os;
	g_wait_str = s;
	if (wait_until(log_has, NULL, 2000))
		return true;
	fprintf(stderr, "    log lacks '%s'\n", s);
	return false;
}

/* Direct top/bottom calls without the mock's dispatcher. */
static void test_irq_top_bottom(void)
{
	struct pw_os *os = new_os(MOCK_FW_OK);
	struct pw_device_params p = { .os = os, .boot_firmware = false };
	struct pw_device *dev = NULL;

	CHECK(!pw_irq_top(NULL, PW_IRQ_GPU));
	REQUIRE(pw_device_create(&p, &dev) == PW_OK);	/* not attached: we drive the IRQs */
	CHECK(!pw_irq_top(dev, (enum pw_irq_line)7));
	/* Lines not set up in M1 mode are never claimed. */
	CHECK(!pw_irq_top(dev, PW_IRQ_JOB));
	CHECK(!pw_irq_top(dev, PW_IRQ_MMU));
	/* Nothing pending: not ours (shared-line safe). */
	CHECK(!pw_irq_top(dev, PW_IRQ_GPU));

	mock_inject_gpu_fault(os, 0x000000c4, 0x0000001234567000ull);
	CHECK(pw_irq_top(dev, PW_IRQ_GPU));
	CHECK_EQ(mock_int_mask(os, PW_IRQ_GPU), 0);	/* ISR masked the line */
	CHECK_EQ(dev->irq[PW_IRQ_GPU].state, PW_IRQ_STATE_PROCESSING);
	/* top again while PROCESSING with the line masked: STAT is 0 -> not ours */
	CHECK(!pw_irq_top(dev, PW_IRQ_GPU));
	pw_irq_bottom(dev, PW_IRQ_GPU);
	CHECK_EQ(dev->irq[PW_IRQ_GPU].state, PW_IRQ_STATE_ACTIVE);
	CHECK_EQ(mock_int_mask(os, PW_IRQ_GPU), 0x20103);	/* restored */
	CHECK(mock_log_contains(os, "GPU Fault 0x000000c4 (TRANSLATION_FAULT_4) at 0x0000001234567000"));
	CHECK_EQ(mock_rawstat(os, PW_IRQ_GPU) & 1, 0);	/* cleared */
	CHECK(!pw_irq_top(dev, PW_IRQ_GPU));

	/* unknown exception code */
	mock_inject_gpu_fault(os, 0x000000ff, 0);
	CHECK(pw_irq_top(dev, PW_IRQ_GPU));
	pw_irq_bottom(dev, PW_IRQ_GPU);
	CHECK(mock_log_contains(os, "(Unknown exception type)"));

	/* a spurious bottom half (OS queued it twice) is harmless */
	pw_irq_bottom(dev, PW_IRQ_GPU);
	CHECK_EQ(dev->irq[PW_IRQ_GPU].state, PW_IRQ_STATE_ACTIVE);

	/*
	 * Review regression: an ISR that sampled PROCESSING (after a racing enable_events
	 * re-wrote INT_MASK) can write INT_MASK = 0 just after the bottom half restored it and
	 * went ACTIVE; it still claims and queues another bottom half. That bottom half must
	 * restore the mask even though the state is already ACTIVE, or the line stays dead.
	 */
	pw_os_reg_write32(os, 0x20 + 0x8, 0);	/* GPU_INT_MASK = 0, state ACTIVE */
	pw_irq_bottom(dev, PW_IRQ_GPU);
	CHECK_EQ(dev->irq[PW_IRQ_GPU].state, PW_IRQ_STATE_ACTIVE);
	CHECK_EQ(mock_int_mask(os, PW_IRQ_GPU), 0x20103);

	pw_device_destroy(dev);
	check_no_leaks(os);
	mock_destroy(os);
}

struct vm_fault_ctx {
	struct pw_vm *vm;
	struct pw_os *os;
};

/* The bottom half marks the VM, then disables the AS (cache flush + UPDATE): wait for both. */
static bool vm_faulted(void *arg)
{
	struct vm_fault_ctx *c = arg;
	struct mock_as_state as;

	mock_get_as(c->os, 1, &as);
	return __atomic_load_n(&c->vm->unhandled_fault, __ATOMIC_ACQUIRE) && !as.enabled &&
	       mock_int_mask(c->os, PW_IRQ_MMU) == 0xfffd;
}

static void test_mmu_fault(void)
{
	struct pw_os *os;
	struct pw_device *dev;
	struct pw_vm *vm;
	struct mock_as_state as;
	struct vm_fault_ctx c;
	uint32_t delivered;

	if (!g_fw_path) {
		test_skip("real firmware not available");
		return;
	}
	os = new_os(MOCK_FW_OK);
	REQUIRE(bringup(os, true, &dev) == PW_OK);
	REQUIRE(pw_vm_create(dev, false, &vm) == PW_OK);
	REQUIRE(pw_vm_activate(vm) == PW_OK);
	CHECK_EQ(vm->as, 1);

	/* REQ_COMPLETE bits (16-31) are never unmasked, so nothing is delivered. */
	delivered = mock_irq_delivered(os, PW_IRQ_MMU);
	mock_raise_mmu_irq_bits(os, 0xffff0000u);
	usleep(20000);
	CHECK_EQ(mock_irq_delivered(os, PW_IRQ_MMU), delivered);

	/* Translation fault, level 1, READ, decoder fault, source 0x1234 */
	mock_inject_mmu_fault(os, 1, 0x123406c1u, 0x12345000ull);
	c.vm = vm;
	c.os = os;
	CHECK(wait_until(vm_faulted, &c, 2000));
	CHECK(wait_log(os, "Unhandled page fault in AS1 at VA 0x0000000012345000"));
	CHECK(wait_log(os, "raw fault status 0x123406C1: DECODER FAULT, exception type 0xC1: "
		       "TRANSLATION_FAULT_1, access type 0x2: READ, source id 0x1234"));
	/* AS disabled (UPDATE to UNMAPPED), fault IRQ for AS1 masked, fault tracked */
	mock_get_as(os, 1, &as);
	CHECK(!as.enabled);
	CHECK_EQ(dev->mmu.faulty_mask, 2);
	CHECK_EQ(dev->irq[PW_IRQ_MMU].mask & 2, 0);
	CHECK_EQ(mock_rawstat(os, PW_IRQ_MMU) & 0xffff, 0);

	/* write / atomic / execute decode on AS0 (MCU VM) */
	mock_inject_mmu_fault(os, 1, 0x000003c8u, 0x1000);	/* masked now: AS1 fault disabled */
	usleep(20000);
	CHECK(!mock_log_contains(os, "PERM_FAULT_0"));
	/* re-activation clears the fault and re-enables the AS (clearing the pending bit) */
	CHECK_EQ(pw_vm_activate(vm), PW_OK);
	CHECK(!vm->unhandled_fault);
	CHECK_EQ(dev->mmu.faulty_mask, 0);
	mock_get_as(os, 1, &as);
	CHECK(as.enabled);
	CHECK_EQ(dev->irq[PW_IRQ_MMU].mask & 2, 2);
	CHECK(!mock_log_contains(os, "PERM_FAULT_0"));	/* the stale fault was cleared */

	mock_inject_mmu_fault(os, 1, 0x000003cbu, 0x2000);
	CHECK(wait_log(os, "exception type 0xCB: PERM_FAULT_3, access type 0x3: WRITE"));
	CHECK(wait_log(os, "SLAVE FAULT"));
	CHECK_EQ(pw_vm_activate(vm), PW_OK);
	mock_inject_mmu_fault(os, 1, 0x000001c3u, 0x3000);
	CHECK(wait_log(os, "access type 0x1: EXECUTE"));
	CHECK_EQ(pw_vm_activate(vm), PW_OK);
	mock_inject_mmu_fault(os, 1, 0x000000d9u, 0x4000);
	CHECK(wait_log(os, "ACCESS_FLAG_1, access type 0x0: ATOMIC"));

	pw_vm_destroy(vm);
	CHECK_EQ(mock_irq_unclaimed(os), 0);
	CHECK_EQ(mock_model_errors(os), 0);
	teardown(os, dev);
}

static bool idle_acked(void *arg)
{
	struct pw_os *os = arg;

	return !((mock_mcu_glb_req(os) ^ mock_mcu_glb_ack(os)) & (1u << 26));
}

static void test_job_irq_glb_event(void)
{
	struct pw_os *os;
	struct pw_device *dev;
	uint32_t req0;

	if (!g_fw_path) {
		test_skip("real firmware not available");
		return;
	}
	os = new_os(MOCK_FW_OK);
	REQUIRE(bringup(os, true, &dev) == PW_OK);
	req0 = mock_mcu_glb_req(os);
	/* FW signals GLB_IDLE by toggling ack; the job bottom half must ack it (sched.c:1855) */
	mock_mcu_raise_glb_event(os, 1u << 26);
	CHECK(wait_until(idle_acked, os, 2000));
	CHECK_EQ((mock_mcu_glb_req(os) ^ req0), 1u << 26);
	/* a GLOBAL_IF with no pending event is harmless */
	mock_mcu_raise_glb_event(os, 0);
	CHECK_EQ(pw_device_fw_ping(dev, 100), PW_OK);
	CHECK_EQ(mock_irq_unclaimed(os), 0);
	teardown(os, dev);
}

static void test_repeated_bringup(void)
{
	if (!g_fw_path) {
		test_skip("real firmware not available");
		return;
	}
	for (int i = 0; i < 20; i++) {
		struct pw_os *os = new_os(MOCK_FW_OK);
		struct pw_device *dev;

		CHECK_EQ(bringup(os, true, &dev), PW_OK);
		CHECK_EQ(mock_model_errors(os), 0);
		teardown(os, dev);
	}
}

const struct test_case device_tests[] = {
	{ "device.bringup_fw_booted", test_bringup },
	{ "device.bringup_without_irqs", test_bringup_no_irq },
	{ "device.bringup_irq_only_completions", test_bringup_irq_completions },
	{ "device.m1_mode", test_m1_mode },
	{ "device.failure_paths", test_failures },
	{ "irq.top_bottom_gpu_fault", test_irq_top_bottom },
	{ "irq.mmu_fault_decode", test_mmu_fault },
	{ "irq.job_glb_idle_event", test_job_irq_glb_event },
	{ "device.repeated_bringup", test_repeated_bringup },
	{ NULL, NULL },
};
