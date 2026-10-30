// SPDX-License-Identifier: MIT
/*
 * test_jm_sched.c - job-manager scheduling: placeholders for the jm-sched module (SUBMIT,
 * JM contexts, dependencies, completion, timeouts, reset). They need the jm-hw executor
 * (mock_jm.c) and the jm-mem BOs; until then they skip. Owner: jm-sched
 * (docs/status/jm-arch.md, tasks C*).
 */
#include "pw_jm_sched.h"
#include "test_jm.h"

static void test_jm_submit_validation(void)
{
	struct jenv e;
	struct drm_panfrost_submit s;
	int ret;

	if (!jenv_init(&e))
		goto out;
	memset(&s, 0, sizeof(s));
	s.jc = 0;
	ret = JIOC(&e, DRM_IOCTL_PANFROST_SUBMIT, &s);
	JM_SKIP_IF_STUB(ret, "SUBMIT", out);
	CHECK_EQ(ret, PW_EINVAL);		/* jc == 0 */
	/* TODO(jm-sched): pad, requirements, out_sync -> ENODEV, jm_ctx_handle, in_syncs EFAULT/
	 * ENOENT/EINVAL, bo_handles ENOENT; nothing user-visible changed on failure. */
out:
	jenv_fini(&e);
}

static void test_jm_ctx(void)
{
	struct jenv e;
	struct drm_panfrost_jm_ctx_create cc;
	struct drm_panfrost_jm_ctx_destroy cd;
	int ret;

	if (!jenv_init(&e))
		goto out;
	memset(&cc, 0, sizeof(cc));
	cc.priority = PANFROST_JM_CTX_PRIORITY_MEDIUM;
	ret = JIOC(&e, DRM_IOCTL_PANFROST_JM_CTX_CREATE, &cc);
	JM_SKIP_IF_STUB(ret, "JM_CTX_CREATE", out);
	CHECK_EQ(ret, PW_OK);
	CHECK(cc.handle != 0);
	memset(&cd, 0, sizeof(cd));
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_JM_CTX_DESTROY, &cd), PW_EINVAL);	/* default */
	cd.handle = cc.handle;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_JM_CTX_DESTROY, &cd), PW_OK);
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_JM_CTX_DESTROY, &cd), PW_EINVAL);
	cc.priority = PANFROST_JM_CTX_PRIORITY_HIGH;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_JM_CTX_CREATE, &cc), PW_EACCES);
	cc.priority = 7;
	CHECK_EQ(JIOC(&e, DRM_IOCTL_PANFROST_JM_CTX_CREATE, &cc), PW_EINVAL);
out:
	jenv_fini(&e);
}

static void test_jm_job_flow(void)
{
	test_skip("needs the mock_jm.c executor + jm-mem BOs (jm-arch tasks A6, B1, C2)");
	/* TODO(jm-sched): WRITE_VALUE chain on JS1 -> out_sync signalled, value written; FS job
	 * on JS0; in_sync dependency ordering; implicit sync through a shared BO (WAIT_BO);
	 * NEXT-register pipelining (2 jobs per slot); fault -> fence error EINVAL; timeout ->
	 * reset, guilty job fails, innocent job resubmitted; JM_CTX_DESTROY kills queued and
	 * running jobs (ECANCELED); file close with jobs in flight; AS LRU with more files than
	 * AS slots (mock_config.as_present); 1000-submit stress, no leaks. */
}

const struct test_case jm_sched_tests[] = {
	{ "jm_submit_validation", test_jm_submit_validation },
	{ "jm_ctx", test_jm_ctx },
	{ "jm_job_flow", test_jm_job_flow },
	{ NULL, NULL },
};
