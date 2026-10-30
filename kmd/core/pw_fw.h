/* SPDX-License-Identifier: MIT */
/*
 * pw_fw.h - CSF firmware image parser (pure, no side effects) used by the loader in pw_fw.c and
 * directly by the unit tests.
 */
#ifndef PW_FW_H
#define PW_FW_H

#include "pw_priv.h"

/* One validated IFACE entry (va/data ranges already bounds-checked against the image). */
struct pw_fw_iface_entry {
	uint32_t flags;
	uint32_t va_start, va_end;
	uint32_t data_start, data_end;
	const uint8_t *name;		/* raw bytes up to the entry end, may contain NULs */
	uint32_t name_len;
	uint32_t entry_offset;		/* offset of the entry header in the image */
};

struct pw_fw_parse_ops {
	/* Called for every IFACE entry that must be loaded (protected-mode and empty sections
	 * are skipped and counted). Return non-zero to abort parsing with that code. */
	int (*iface)(void *ctx, const struct pw_fw_iface_entry *e, const uint8_t *image);
};

struct pw_fw_image_info {
	uint8_t major, minor;
	uint32_t version_hash;
	uint32_t hdr_size;
	uint32_t n_entries;
	uint32_t n_iface;		/* IFACE entries seen (incl. skipped) */
	uint32_t n_iface_loaded;	/* passed to ops->iface */
	uint32_t n_iface_prot;		/* protected-mode sections, skipped */
	uint32_t n_config;
	uint32_t n_trace;
	uint32_t n_other;		/* optional entries of unknown type, skipped */
	bool has_shared;
	uint64_t total_size;		/* sum of loaded section sizes */
	char git_sha[48];		/* "" if no usable BUILD_INFO entry */
};

#define PW_FW_PAGE_SIZE		0x1000u
#define PW_FW_MAX_ENTRIES	256
/* Sanity caps (not in panthor): real arch10.8 sections are <= 256 KiB, 7 sections total. */
#define PW_FW_MAX_SECTION_SIZE	(32u << 20)
#define PW_FW_MAX_TOTAL_SIZE	(64u << 20)

int pw_fw_parse(struct pw_os *os, const void *data, size_t size, const struct pw_fw_parse_ops *ops,
		void *ctx, struct pw_fw_image_info *info);

#endif /* PW_FW_H */
