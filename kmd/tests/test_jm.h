/* SPDX-License-Identifier: MIT */
/*
 * test_jm.h - shared fixture for the job-manager suites (test_jm_hw.c, test_jm_mem.c,
 * test_jm_sched.c): a mock G31 (MOCK_MODEL_G31), probed the way the Windows glue does it
 * (alloc, attach IRQs, probe), plus one open file. Header-only so each suite stays one file.
 * Owner: jm-arch; the three JM engineers may add helpers here (append, do not change).
 */
#ifndef PW_TEST_JM_H
#define PW_TEST_JM_H

#include <string.h>

#include "mock_gpu.h"
#include "panfrost_drm.h"
#include "pw_core.h"
#include "test.h"

struct jenv {
	struct pw_os *os;
	struct pw_device *dev;
	struct pw_file *file;
};

/* @cfg may be NULL (plain G31); its model is forced to MOCK_MODEL_G31. */
static inline bool jenv_init_cfg(struct jenv *e, const struct mock_config *cfg)
{
	struct mock_config c = { 0 };
	struct pw_device_params p = { 0 };

	memset(e, 0, sizeof(*e));
	if (cfg)
		c = *cfg;
	c.model = MOCK_MODEL_G31;
	e->os = mock_create(&c);
	p.os = e->os;
	p.boot_firmware = false;	/* ignored on JM */
	if (pw_device_alloc(&p, &e->dev)) {
		CHECK(!"device alloc failed");
		return false;
	}
	mock_attach_irq(e->os, e->dev);
	if (pw_device_probe(e->dev)) {
		CHECK(!"G31 probe failed");
		return false;
	}
	if (pw_file_open(e->dev, &e->file)) {
		CHECK(!"file open failed");
		return false;
	}
	return true;
}

static inline bool jenv_init(struct jenv *e)
{
	return jenv_init_cfg(e, NULL);
}

static inline void jenv_fini(struct jenv *e)
{
	if (e->file)
		pw_file_close(e->file);
	e->file = NULL;
	mock_detach_irq(e->os);
	pw_device_destroy(e->dev);
	check_no_leaks(e->os);
	CHECK_EQ(mock_model_errors(e->os), 0);
	CHECK_EQ(mock_irq_unclaimed(e->os), 0);
	mock_destroy(e->os);
}

/* ioctl(fd, req, arg) equivalent: size from the Linux ioctl number, like the OS layer. */
#define JIOC(e, req, arg) pw_file_ioctl((e)->file, _IOC_NR(req), (arg), _IOC_SIZE(req))

/* Placeholder suites: skip (not fail) while a module is still the jm-arch stub; jumps to
 * @out so the fixture is still torn down. Remove the uses once the module is implemented. */
#define JM_SKIP_IF_STUB(ret, what, out) do {					\
		if ((ret) == PW_ENOTTY) {						\
			test_skip(what " not implemented yet (jm-arch stub)");		\
			goto out;							\
		}									\
	} while (0)

#endif /* PW_TEST_JM_H */
