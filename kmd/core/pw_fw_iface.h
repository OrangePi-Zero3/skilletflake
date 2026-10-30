/* SPDX-License-Identifier: MIT */
/*
 * pw_fw_iface.h - CSF firmware binary format and host<->FW shared-memory interface layouts.
 *
 * Derived from Linux panthor (dual GPL-2.0/MIT, used under MIT):
 *   drivers/gpu/drm/panthor/panthor_fw.h  (interface structs, req/ack bits; Copyright 2023 Collabora)
 *   drivers/gpu/drm/panthor/panthor_fw.c:40-240 (binary header / entry format, iface constants)
 * The structs are shared with the MCU, so their layout is ABI: offsets are pinned with
 * static asserts below (checked against panthor_fw.h compiled with Linux types, see
 * docs/status/kmd-core.md).
 */
#ifndef PW_FW_IFACE_H
#define PW_FW_IFACE_H

#include "pw_util.h"

/* ------------------------------------------------------------------------------------------ */
/* Firmware binary (mali_csffw.bin) format, panthor_fw.c:40-200                               */
/* ------------------------------------------------------------------------------------------ */

struct pw_fw_binary_hdr {
	uint32_t magic;
#define CSF_FW_BINARY_HEADER_MAGIC		0xc3f13a6eu
	uint8_t  minor;
	uint8_t  major;
#define CSF_FW_BINARY_HEADER_MAJOR_MAX		0
	uint16_t padding1;
	uint32_t version_hash;
	uint32_t padding2;
	uint32_t size;		/* size of the header + entry area, not of the file */
};

enum pw_fw_binary_entry_type {
	CSF_FW_BINARY_ENTRY_TYPE_IFACE = 0,
	CSF_FW_BINARY_ENTRY_TYPE_CONFIG = 1,
	CSF_FW_BINARY_ENTRY_TYPE_FUTF_TEST = 2,
	CSF_FW_BINARY_ENTRY_TYPE_TRACE_BUFFER = 3,
	CSF_FW_BINARY_ENTRY_TYPE_TIMELINE_METADATA = 4,
	CSF_FW_BINARY_ENTRY_TYPE_BUILD_INFO_METADATA = 6,
};

#define CSF_FW_BINARY_ENTRY_TYPE(ehdr)		((ehdr) & 0xffu)
#define CSF_FW_BINARY_ENTRY_SIZE(ehdr)		(((ehdr) >> 8) & 0xffu)	/* bytes, incl. ehdr */
#define CSF_FW_BINARY_ENTRY_UPDATE		PW_BIT(30)
#define CSF_FW_BINARY_ENTRY_OPTIONAL		PW_BIT(31)

#define CSF_FW_BINARY_IFACE_ENTRY_RD				PW_BIT(0)
#define CSF_FW_BINARY_IFACE_ENTRY_WR				PW_BIT(1)
#define CSF_FW_BINARY_IFACE_ENTRY_EX				PW_BIT(2)
#define CSF_FW_BINARY_IFACE_ENTRY_CACHE_MODE_NONE		(0u << 3)
#define CSF_FW_BINARY_IFACE_ENTRY_CACHE_MODE_CACHED		(1u << 3)
#define CSF_FW_BINARY_IFACE_ENTRY_CACHE_MODE_UNCACHED_COHERENT	(2u << 3)
#define CSF_FW_BINARY_IFACE_ENTRY_CACHE_MODE_CACHED_COHERENT	(3u << 3)
#define CSF_FW_BINARY_IFACE_ENTRY_CACHE_MODE_MASK		PW_GENMASK(4, 3)
#define CSF_FW_BINARY_IFACE_ENTRY_PROT				PW_BIT(5)
#define CSF_FW_BINARY_IFACE_ENTRY_SHARED			PW_BIT(30)
#define CSF_FW_BINARY_IFACE_ENTRY_ZERO				PW_BIT(31)

#define CSF_FW_BINARY_IFACE_ENTRY_SUPPORTED_FLAGS		\
	(CSF_FW_BINARY_IFACE_ENTRY_RD |				\
	 CSF_FW_BINARY_IFACE_ENTRY_WR |				\
	 CSF_FW_BINARY_IFACE_ENTRY_EX |				\
	 CSF_FW_BINARY_IFACE_ENTRY_CACHE_MODE_MASK |		\
	 CSF_FW_BINARY_IFACE_ENTRY_PROT |			\
	 CSF_FW_BINARY_IFACE_ENTRY_SHARED |			\
	 CSF_FW_BINARY_IFACE_ENTRY_ZERO)

struct pw_fw_binary_section_entry_hdr {
	uint32_t flags;
	struct {
		uint32_t start;
		uint32_t end;
	} va;			/* MCU VA range */
	struct {
		uint32_t start;
		uint32_t end;
	} data;			/* offsets in the binary of the initial contents */
	/* followed by an optional, not necessarily NUL-terminated name up to the entry end */
};

struct pw_fw_build_info_hdr {
	uint32_t meta_start;
	uint32_t meta_size;
};

#define CSF_MCU_SHARED_REGION_START		0x04000000ull
#define CSF_MCU_SHARED_REGION_SIZE		0x04000000ull

#define MIN_CS_PER_CSG				8
#define MIN_CSGS				3
#define MAX_CSGS				31
#define MAX_CS_PER_CSG				32
#define MAX_CSG_PRIO				0xf	/* panthor_sched.c:96 */

#define CSF_IFACE_VERSION(major, minor, patch)	\
	(((uint32_t)(major) << 24) | ((uint32_t)(minor) << 16) | (uint32_t)(patch))
#define CSF_IFACE_VERSION_MAJOR(v)		((v) >> 24)
#define CSF_IFACE_VERSION_MINOR(v)		(((v) >> 16) & 0xff)
#define CSF_IFACE_VERSION_PATCH(v)		((v) & 0xffff)

#define CSF_GROUP_CONTROL_OFFSET		0x1000
#define CSF_STREAM_CONTROL_OFFSET		0x40
#define CSF_UNPRESERVED_REG_COUNT		4

/* ------------------------------------------------------------------------------------------ */
/* Shared interface structures, panthor_fw.h                                                  */
/* ------------------------------------------------------------------------------------------ */

struct pw_fw_ringbuf_input_iface {
	uint64_t insert;
	uint64_t extract;
};

struct pw_fw_ringbuf_output_iface {
	uint64_t extract;
	uint32_t active;
};

struct pw_fw_cs_control_iface {
#define CS_FEATURES_WORK_REGS(x)		(((x) & PW_GENMASK(7, 0)) + 1)
#define CS_FEATURES_SCOREBOARDS(x)		(((x) & PW_GENMASK(15, 8)) >> 8)
#define CS_FEATURES_COMPUTE			PW_BIT(16)
#define CS_FEATURES_FRAGMENT			PW_BIT(17)
#define CS_FEATURES_TILER			PW_BIT(18)
	uint32_t features;
	uint32_t input_va;
	uint32_t output_va;
};

struct pw_fw_cs_input_iface {
#define CS_STATE_MASK				PW_GENMASK(2, 0)
#define CS_STATE_STOP				0
#define CS_STATE_START				1
#define CS_EXTRACT_EVENT			PW_BIT(4)
#define CS_IDLE_SYNC_WAIT			PW_BIT(8)
#define CS_IDLE_PROTM_PENDING			PW_BIT(9)
#define CS_IDLE_EMPTY				PW_BIT(10)
#define CS_IDLE_RESOURCE_REQ			PW_BIT(11)
#define CS_TILER_OOM				PW_BIT(26)
#define CS_PROTM_PENDING			PW_BIT(27)
#define CS_FATAL				PW_BIT(30)
#define CS_FAULT				PW_BIT(31)
#define CS_REQ_MASK				(CS_STATE_MASK | CS_EXTRACT_EVENT | \
						 CS_IDLE_SYNC_WAIT | CS_IDLE_PROTM_PENDING | \
						 CS_IDLE_EMPTY | CS_IDLE_RESOURCE_REQ)
#define CS_EVT_MASK				(CS_TILER_OOM | CS_PROTM_PENDING | \
						 CS_FATAL | CS_FAULT)
	uint32_t req;
#define CS_CONFIG_PRIORITY(x)			((x) & PW_GENMASK(3, 0))
#define CS_CONFIG_DOORBELL(x)			(((x) << 8) & PW_GENMASK(15, 8))
	uint32_t config;
	uint32_t reserved1;
	uint32_t ack_irq_mask;
	uint64_t ringbuf_base;
	uint32_t ringbuf_size;
	uint32_t reserved2;
	uint64_t heap_start;
	uint64_t heap_end;
	uint64_t ringbuf_input;
	uint64_t ringbuf_output;
	uint32_t instr_config;
	uint32_t instrbuf_size;
	uint64_t instrbuf_base;
	uint64_t instrbuf_offset_ptr;
};

struct pw_fw_cs_output_iface {
	uint32_t ack;
	uint32_t reserved1[15];
	uint64_t status_cmd_ptr;
#define CS_STATUS_WAIT_SB_MASK			PW_GENMASK(15, 0)
#define CS_STATUS_WAIT_SB_SRC_MASK		PW_GENMASK(19, 16)
#define CS_STATUS_WAIT_SB_SRC_NONE		(0u << 16)
#define CS_STATUS_WAIT_SB_SRC_WAIT		(8u << 16)
#define CS_STATUS_WAIT_SYNC_COND_LE		(0u << 24)
#define CS_STATUS_WAIT_SYNC_COND_GT		(1u << 24)
#define CS_STATUS_WAIT_SYNC_COND_MASK		PW_GENMASK(27, 24)
#define CS_STATUS_WAIT_PROGRESS			PW_BIT(28)
#define CS_STATUS_WAIT_PROTM			PW_BIT(29)
#define CS_STATUS_WAIT_SYNC_64B			PW_BIT(30)
#define CS_STATUS_WAIT_SYNC			PW_BIT(31)
	uint32_t status_wait;
	uint32_t status_req_resource;
	uint64_t status_wait_sync_ptr;
	uint32_t status_wait_sync_value;
	uint32_t status_scoreboards;
#define CS_STATUS_BLOCKED_REASON_UNBLOCKED	0
#define CS_STATUS_BLOCKED_REASON_SB_WAIT	1
#define CS_STATUS_BLOCKED_REASON_PROGRESS_WAIT	2
#define CS_STATUS_BLOCKED_REASON_SYNC_WAIT	3
#define CS_STATUS_BLOCKED_REASON_DEFERRED	4
#define CS_STATUS_BLOCKED_REASON_RESOURCE	5
#define CS_STATUS_BLOCKED_REASON_FLUSH		6
#define CS_STATUS_BLOCKED_REASON_MASK		PW_GENMASK(3, 0)
	uint32_t status_blocked_reason;
	uint32_t status_wait_sync_value_hi;
	uint32_t reserved2[6];
#define CS_EXCEPTION_TYPE(x)			((x) & PW_GENMASK(7, 0))
#define CS_EXCEPTION_DATA(x)			(((x) >> 8) & PW_GENMASK(23, 0))
	uint32_t fault;
	uint32_t fatal;
	uint64_t fault_info;
	uint64_t fatal_info;
	uint32_t reserved3[10];
	uint32_t heap_vt_start;
	uint32_t heap_vt_end;
	uint32_t reserved4;
	uint32_t heap_frag_end;
	uint64_t heap_address;
};

struct pw_fw_csg_control_iface {
	uint32_t features;
	uint32_t input_va;
	uint32_t output_va;
	uint32_t suspend_size;
	uint32_t protm_suspend_size;
	uint32_t stream_num;
	uint32_t stream_stride;
};

struct pw_fw_csg_input_iface {
#define CSG_STATE_MASK				PW_GENMASK(2, 0)
#define CSG_STATE_TERMINATE			0
#define CSG_STATE_START				1
#define CSG_STATE_SUSPEND			2
#define CSG_STATE_RESUME			3
#define CSG_ENDPOINT_CONFIG			PW_BIT(4)
#define CSG_STATUS_UPDATE			PW_BIT(5)
#define CSG_SYNC_UPDATE				PW_BIT(28)
#define CSG_IDLE				PW_BIT(29)
#define CSG_DOORBELL				PW_BIT(30)
#define CSG_PROGRESS_TIMER_EVENT		PW_BIT(31)
#define CSG_REQ_MASK				(CSG_STATE_MASK | CSG_ENDPOINT_CONFIG | \
						 CSG_STATUS_UPDATE)
#define CSG_EVT_MASK				(CSG_SYNC_UPDATE | CSG_IDLE | \
						 CSG_PROGRESS_TIMER_EVENT)
	uint32_t req;
	uint32_t ack_irq_mask;
	uint32_t doorbell_req;
	uint32_t cs_irq_ack;
	uint32_t reserved1[4];
	uint64_t allow_compute;
	uint64_t allow_fragment;
	uint32_t allow_other;
#define CSG_EP_REQ_COMPUTE(x)			((x) & PW_GENMASK(7, 0))
#define CSG_EP_REQ_FRAGMENT(x)			(((x) << 8) & PW_GENMASK(15, 8))
#define CSG_EP_REQ_TILER(x)			(((x) << 16) & PW_GENMASK(19, 16))
#define CSG_EP_REQ_EXCL_COMPUTE			PW_BIT(20)
#define CSG_EP_REQ_EXCL_FRAGMENT		PW_BIT(21)
#define CSG_EP_REQ_PRIORITY_MASK		PW_GENMASK(31, 28)
#define CSG_EP_REQ_PRIORITY(x)			(((x) << 28) & CSG_EP_REQ_PRIORITY_MASK)
#define CSG_EP_REQ_PRIORITY_GET(x)		(((x) & CSG_EP_REQ_PRIORITY_MASK) >> 28)
	uint32_t endpoint_req;
	uint64_t endpoint_req2;		/* iface >= 4.0 (fw.c:334) */
	uint64_t suspend_buf;
	uint64_t protm_suspend_buf;
	uint32_t config;
	uint32_t iter_trace_config;
};

struct pw_fw_csg_output_iface {
	uint32_t ack;
	uint32_t reserved1;
	uint32_t doorbell_ack;
	uint32_t cs_irq_req;
	uint32_t status_endpoint_current;
	uint32_t status_endpoint_req;
#define CSG_STATUS_STATE_IS_IDLE		PW_BIT(0)
	uint32_t status_state;
	uint32_t resource_dep;
};

struct pw_fw_global_control_iface {
	uint32_t version;
	uint32_t features;
	uint32_t input_va;
	uint32_t output_va;
	uint32_t group_num;
	uint32_t group_stride;
	uint32_t perfcnt_size;
	uint32_t instr_features;
};

struct pw_fw_global_input_iface {
#define GLB_HALT				PW_BIT(0)
#define GLB_CFG_PROGRESS_TIMER			PW_BIT(1)
#define GLB_CFG_ALLOC_EN			PW_BIT(2)
#define GLB_CFG_POWEROFF_TIMER			PW_BIT(3)
#define GLB_PROTM_ENTER				PW_BIT(4)
#define GLB_PERFCNT_EN				PW_BIT(5)
#define GLB_PERFCNT_SAMPLE			PW_BIT(6)
#define GLB_COUNTER_EN				PW_BIT(7)
#define GLB_PING				PW_BIT(8)
#define GLB_FWCFG_UPDATE			PW_BIT(9)
#define GLB_IDLE_EN				PW_BIT(10)
#define GLB_SLEEP				PW_BIT(12)
#define GLB_STATE_MASK				PW_GENMASK(14, 12)
#define   GLB_STATE_ACTIVE			0
#define   GLB_STATE_HALT			1
#define   GLB_STATE_SLEEP			2
#define   GLB_STATE_SUSPEND			3
#define   GLB_STATE(x)				(((x) << 12) & GLB_STATE_MASK)
#define   GLB_STATE_GET(x)			(((x) & GLB_STATE_MASK) >> 12)
#define GLB_INACTIVE_COMPUTE			PW_BIT(20)
#define GLB_INACTIVE_FRAGMENT			PW_BIT(21)
#define GLB_INACTIVE_TILER			PW_BIT(22)
#define GLB_PROTM_EXIT				PW_BIT(23)
#define GLB_PERFCNT_THRESHOLD			PW_BIT(24)
#define GLB_PERFCNT_OVERFLOW			PW_BIT(25)
#define GLB_IDLE				PW_BIT(26)
#define GLB_DBG_CSF				PW_BIT(30)
#define GLB_DBG_HOST				PW_BIT(31)
#define GLB_REQ_MASK				PW_GENMASK(10, 0)
#define GLB_EVT_MASK				PW_GENMASK(26, 20)
	uint32_t req;
	uint32_t ack_irq_mask;
	uint32_t doorbell_req;
	uint32_t reserved1;
	uint32_t progress_timer;
#define GLB_TIMER_VAL(x)			((x) & PW_GENMASK(30, 0))
#define GLB_TIMER_SOURCE_GPU_COUNTER		PW_BIT(31)
	uint32_t poweroff_timer;
	uint64_t core_en_mask;
	uint32_t reserved2;
	uint32_t perfcnt_as;
	uint64_t perfcnt_base;
	uint32_t perfcnt_extract;
	uint32_t reserved3[3];
	uint32_t perfcnt_config;
	uint32_t perfcnt_csg_select;
	uint32_t perfcnt_fw_enable;
	uint32_t perfcnt_csg_enable;
	uint32_t perfcnt_csf_enable;
	uint32_t perfcnt_shader_enable;
	uint32_t perfcnt_tiler_enable;
	uint32_t perfcnt_mmu_l2_enable;
	uint32_t reserved4[8];
	uint32_t idle_timer;
};

enum pw_fw_halt_status {
	PW_FW_HALT_OK = 0,
	PW_FW_HALT_ON_PANIC = 0x4e,
	PW_FW_HALT_ON_WATCHDOG_EXPIRATION = 0x4f,
};

struct pw_fw_global_output_iface {
	uint32_t ack;
	uint32_t reserved1;
	uint32_t doorbell_ack;
	uint32_t reserved2;
	uint32_t halt_status;
	uint32_t perfcnt_status;
	uint32_t perfcnt_insert;
};

/* Layout pins: values computed from panthor_fw.h with Linux u32/u64 types (LP64 and LLP64 agree
 * because every member is fixed-width and naturally aligned). */
#define PW_FW_OFS(t, m) __builtin_offsetof(struct t, m)
_Static_assert(sizeof(struct pw_fw_binary_hdr) == 20, "fw hdr");
_Static_assert(sizeof(struct pw_fw_binary_section_entry_hdr) == 20, "section hdr");
_Static_assert(sizeof(struct pw_fw_cs_control_iface) == 0xc, "cs control");
_Static_assert(PW_FW_OFS(pw_fw_cs_input_iface, ringbuf_base) == 0x10, "cs input");
_Static_assert(PW_FW_OFS(pw_fw_cs_input_iface, ringbuf_input) == 0x30, "cs input");
_Static_assert(PW_FW_OFS(pw_fw_cs_input_iface, instrbuf_offset_ptr) == 0x50, "cs input");
_Static_assert(sizeof(struct pw_fw_cs_input_iface) == 0x58, "cs input");
_Static_assert(PW_FW_OFS(pw_fw_cs_output_iface, status_cmd_ptr) == 0x40, "cs output");
_Static_assert(PW_FW_OFS(pw_fw_cs_output_iface, fault) == 0x80, "cs output");
_Static_assert(PW_FW_OFS(pw_fw_cs_output_iface, heap_vt_start) == 0xc0, "cs output");
_Static_assert(PW_FW_OFS(pw_fw_cs_output_iface, heap_address) == 0xd0, "cs output");
_Static_assert(sizeof(struct pw_fw_cs_output_iface) == 0xd8, "cs output");
_Static_assert(sizeof(struct pw_fw_csg_control_iface) == 0x1c, "csg control");
_Static_assert(PW_FW_OFS(pw_fw_csg_input_iface, allow_compute) == 0x20, "csg input");
_Static_assert(PW_FW_OFS(pw_fw_csg_input_iface, endpoint_req) == 0x34, "csg input");
_Static_assert(PW_FW_OFS(pw_fw_csg_input_iface, endpoint_req2) == 0x38, "csg input");
_Static_assert(PW_FW_OFS(pw_fw_csg_input_iface, config) == 0x50, "csg input");
_Static_assert(sizeof(struct pw_fw_csg_input_iface) == 0x58, "csg input");
_Static_assert(sizeof(struct pw_fw_csg_output_iface) == 0x20, "csg output");
_Static_assert(sizeof(struct pw_fw_global_control_iface) == 0x20, "glb control");
_Static_assert(PW_FW_OFS(pw_fw_global_input_iface, core_en_mask) == 0x18, "glb input");
_Static_assert(PW_FW_OFS(pw_fw_global_input_iface, perfcnt_base) == 0x28, "glb input");
_Static_assert(PW_FW_OFS(pw_fw_global_input_iface, perfcnt_config) == 0x40, "glb input");
_Static_assert(PW_FW_OFS(pw_fw_global_input_iface, idle_timer) == 0x80, "glb input");
_Static_assert(sizeof(struct pw_fw_global_input_iface) == 0x88, "glb input");
_Static_assert(sizeof(struct pw_fw_global_output_iface) == 0x1c, "glb output");

#endif /* PW_FW_IFACE_H */
