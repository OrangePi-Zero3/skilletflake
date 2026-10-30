// SPDX-License-Identifier: MIT
/* pw_handle.c - handle tables (idr/xarray replacement): small ints >= 1 -> object pointers. */

#include "pw_file.h"

int pw_handle_table_init(struct pw_os *os, struct pw_handle_table *t)
{
	pw_memset(t, 0, sizeof(*t));
	t->lock = pw_os_mutex_create(os);
	return t->lock ? PW_OK : PW_ENOMEM;
}

void pw_handle_table_fini(struct pw_os *os, struct pw_handle_table *t, void (*release)(void *obj))
{
	for (uint32_t i = 0; i < t->cap; i++) {
		if (t->objs[i] && release)
			release(t->objs[i]);
	}
	pw_os_free(os, t->objs);
	if (t->lock)
		pw_os_mutex_destroy(os, t->lock);
	pw_memset(t, 0, sizeof(*t));
}

int pw_handle_alloc(struct pw_os *os, struct pw_handle_table *t, void *obj, uint32_t *handle)
{
	uint32_t i;

	pw_os_mutex_lock(t->lock);
	for (i = 0; i < t->cap && t->objs[i]; i++)
		;
	if (i == t->cap) {
		uint32_t ncap = t->cap ? t->cap * 2 : 16;
		void **n;

		if (ncap > (1u << 24)) {
			pw_os_mutex_unlock(t->lock);
			return PW_ENOSPC;
		}
		n = pw_os_zalloc(os, ncap * sizeof(*n));
		if (!n) {
			pw_os_mutex_unlock(t->lock);
			return PW_ENOMEM;
		}
		if (t->cap)
			pw_memcpy(n, t->objs, t->cap * sizeof(*n));
		pw_os_free(os, t->objs);
		t->objs = n;
		t->cap = ncap;
	}
	t->objs[i] = obj;
	t->count++;
	*handle = i + 1;
	pw_os_mutex_unlock(t->lock);
	return PW_OK;
}

void *pw_handle_get(struct pw_handle_table *t, uint32_t handle, void (*get)(void *obj))
{
	void *obj = NULL;

	pw_os_mutex_lock(t->lock);
	if (handle && handle <= t->cap) {
		obj = t->objs[handle - 1];
		if (obj && get)
			get(obj);
	}
	pw_os_mutex_unlock(t->lock);
	return obj;
}

void *pw_handle_remove(struct pw_handle_table *t, uint32_t handle)
{
	void *obj = NULL;

	pw_os_mutex_lock(t->lock);
	if (handle && handle <= t->cap && t->objs[handle - 1]) {
		obj = t->objs[handle - 1];
		t->objs[handle - 1] = NULL;
		t->count--;
	}
	pw_os_mutex_unlock(t->lock);
	return obj;
}

void *pw_handle_replace(struct pw_handle_table *t, uint32_t handle, void *obj)
{
	void *old = NULL;

	pw_os_mutex_lock(t->lock);
	if (handle && handle <= t->cap && t->objs[handle - 1]) {
		old = t->objs[handle - 1];
		t->objs[handle - 1] = obj;
	}
	pw_os_mutex_unlock(t->lock);
	return old;
}
