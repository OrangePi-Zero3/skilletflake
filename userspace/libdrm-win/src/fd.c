/* SPDX-License-Identifier: MIT */
/*
 * File-descriptor, mmap and transport emulation for libdrm-win.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "panwin/panwin_ioctl.h"
#include "libdrm_win.h"
#include "pw_internal.h"

/* ---- transport registry and selection ---- */

#define PW_MAX_TRANSPORTS 8

static SRWLOCK fd_lock = SRWLOCK_INIT;      /* protects fds[], open_count, g_tp, transports[] */
static struct pw_file *fds[PANWIN_FD_MAX];
static int open_count;                      /* live pw_file objects (fds + mappings) */
static const struct panwin_transport *g_tp;
static const struct panwin_transport *transports[PW_MAX_TRANSPORTS] = {
	&pw_transport_win32,
#ifdef PANWIN_HAVE_FAKE_KERNEL
	&pw_transport_fake,
#endif
#ifdef PANWIN_HAVE_CORE_TRANSPORT
	&pw_transport_core,
#endif
};

static bool transport_valid(const struct panwin_transport *tp)
{
	return tp && tp->version == PANWIN_TRANSPORT_VERSION && tp->name && tp->name[0] &&
	       tp->enumerate && tp->device_path && tp->open && tp->close && tp->ioctl && tp->os_handle;
}

/* Caller holds fd_lock (shared or exclusive). */
static const struct panwin_transport *find_transport_locked(const char *name)
{
	for (int i = 0; i < PW_MAX_TRANSPORTS && transports[i]; i++)
		if (!strcmp(transports[i]->name, name))
			return transports[i];
	return NULL;
}

int panwin_register_transport(const struct panwin_transport *tp)
{
	int r = 0;

	if (!transport_valid(tp)) {
		errno = EINVAL;
		return -1;
	}
	AcquireSRWLockExclusive(&fd_lock);
	if (find_transport_locked(tp->name)) {
		r = EEXIST;
	} else {
		r = ENOSPC;
		for (int i = 0; i < PW_MAX_TRANSPORTS; i++) {
			if (!transports[i]) {
				transports[i] = tp;
				r = 0;
				break;
			}
		}
	}
	ReleaseSRWLockExclusive(&fd_lock);
	if (r) {
		errno = r;
		return -1;
	}
	return 0;
}

/* "dll:<path>": load a transport DLL and register what its entry point returns. */
static const struct panwin_transport *load_transport_dll(const char *path)
{
	HMODULE m = LoadLibraryA(path);
	panwin_transport_entry_fn entry;
	const struct panwin_transport *tp;

	if (!m)
		return NULL;
	entry = (panwin_transport_entry_fn)(void (*)(void))GetProcAddress(m, PANWIN_TRANSPORT_ENTRY);
	tp = entry ? entry() : NULL;
	if (!transport_valid(tp)) {
		FreeLibrary(m);
		return NULL;
	}
	/* The DLL stays loaded for the life of the process (the table lives in it). */
	if (panwin_register_transport(tp) && errno != EEXIST)
		return NULL;
	AcquireSRWLockShared(&fd_lock);
	tp = find_transport_locked(tp->name);
	ReleaseSRWLockShared(&fd_lock);
	return tp;
}

static const struct panwin_transport *lookup_transport(const char *name)
{
	const struct panwin_transport *tp;

	if (!strncmp(name, "dll:", 4))
		return load_transport_dll(name + 4);
	AcquireSRWLockShared(&fd_lock);
	tp = find_transport_locked(name);
	ReleaseSRWLockShared(&fd_lock);
	return tp;
}

static const struct panwin_transport *select_default_transport(void)
{
	char v[MAX_PATH + 8];
	DWORD n = GetEnvironmentVariableA("PANWIN_TRANSPORT", v, sizeof(v));
	const struct panwin_transport *tp;

	if (n > 0 && n < sizeof(v)) {
		tp = lookup_transport(v);
		if (tp)
			return tp;
		fprintf(stderr, "libdrm-win: PANWIN_TRANSPORT=%s: no such transport, using \"real\"\n", v);
		return &pw_transport_win32;
	}
#ifdef PANWIN_HAVE_FAKE_KERNEL
	n = GetEnvironmentVariableA("PANWIN_FAKE_KERNEL", v, sizeof(v));
	if (n > 0 && n < sizeof(v) && v[0] != '0')
		return &pw_transport_fake;
#endif
	return &pw_transport_win32;
}

const struct panwin_transport *pw_transport(void)
{
	const struct panwin_transport *tp;

	AcquireSRWLockShared(&fd_lock);
	tp = g_tp;
	ReleaseSRWLockShared(&fd_lock);
	if (tp)
		return tp;

	tp = select_default_transport(); /* may load a DLL: not under fd_lock */
	AcquireSRWLockExclusive(&fd_lock);
	if (!g_tp)
		g_tp = tp;
	tp = g_tp;
	ReleaseSRWLockExclusive(&fd_lock);
	return tp;
}

int panwin_select_transport(const char *name)
{
	const struct panwin_transport *tp = name ? lookup_transport(name) : NULL;
	int r = 0;

	if (!tp) {
		errno = ENOENT;
		return -1;
	}
	AcquireSRWLockExclusive(&fd_lock);
	if (open_count && g_tp != tp)
		r = EBUSY;
	else
		g_tp = tp;
	ReleaseSRWLockExclusive(&fd_lock);
	if (r) {
		errno = r;
		return -1;
	}
	return 0;
}

int panwin_use_fake_kernel(int enable)
{
#ifdef PANWIN_HAVE_FAKE_KERNEL
	return panwin_select_transport(enable ? "fake" : "real");
#else
	(void)enable;
	errno = ENOSYS;
	return -1;
#endif
}

int panwin_device_path(int index, char *buf, size_t len)
{
	const struct panwin_transport *tp = pw_transport();
	int r;

	tp->enumerate();
	r = tp->device_path(index, buf, len);
	if (r) {
		errno = -r;
		return -1;
	}
	return 0;
}

const char *panwin_transport_name(void)
{
	return pw_transport()->name;
}

/* ---- synthetic node paths ---- */

/* "/dev/dri/renderD<128+i>" (type DRM_NODE_RENDER=2) or "/dev/dri/card<i>" (DRM_NODE_PRIMARY=0). */
int pw_parse_node(const char *path, int *index, int *node_type)
{
	static const char dir[] = "/dev/dri/";
	const char *p;
	char *end;
	long n;
	int type, base;

	if (!path || strncmp(path, dir, sizeof(dir) - 1))
		return -ENOENT;
	p = path + sizeof(dir) - 1;
	if (!strncmp(p, "renderD", 7)) {
		p += 7;
		type = 2;
		base = PW_RENDER_MINOR_BASE;
	} else if (!strncmp(p, "card", 4)) {
		p += 4;
		type = 0;
		base = PW_PRIMARY_MINOR_BASE;
	} else {
		return -ENOENT;
	}
	if (*p < '0' || *p > '9')
		return -ENOENT;
	n = strtol(p, &end, 10);
	if (*end || n < base || n >= base + PW_MAX_DEVICES)
		return -ENOENT;
	*index = (int)(n - base);
	*node_type = type;
	return 0;
}

void pw_format_node(char *buf, size_t len, int index, int node_type)
{
	if (node_type == 2)
		snprintf(buf, len, "/dev/dri/renderD%d", PW_RENDER_MINOR_BASE + index);
	else
		snprintf(buf, len, "/dev/dri/card%d", PW_PRIMARY_MINOR_BASE + index);
}

int panwin_stat_rdev(const char *path, uint64_t *rdev)
{
	int index, type, n;

	if (pw_parse_node(path, &index, &type)) {
		errno = ENOENT;
		return -1;
	}
	n = pw_transport()->enumerate();
	if (n <= index) {
		errno = ENOENT;
		return -1;
	}
	if (rdev)
		*rdev = PANWIN_MAKEDEV(PANWIN_DRM_MAJOR,
				       (type == 2 ? PW_RENDER_MINOR_BASE : PW_PRIMARY_MINOR_BASE) + index);
	return 0;
}

/* ---- files and fds ---- */

static void file_ref(struct pw_file *f)
{
	InterlockedIncrement(&f->refs);
}

void pw_file_put(struct pw_file *f)
{
	if (InterlockedDecrement(&f->refs) == 0) {
		f->tp->close(f->tfile);
		AcquireSRWLockExclusive(&fd_lock);
		open_count--;
		ReleaseSRWLockExclusive(&fd_lock);
		free(f);
	}
}

struct pw_file *pw_file_get(int fd)
{
	struct pw_file *f = NULL;
	unsigned slot = (unsigned)(fd - PANWIN_FD_BASE);

	if (slot < PANWIN_FD_MAX) {
		AcquireSRWLockShared(&fd_lock);
		f = fds[slot];
		if (f)
			file_ref(f);
		ReleaseSRWLockShared(&fd_lock);
	}
	if (!f)
		errno = EBADF;
	return f;
}

/* Install f in the lowest free slot; consumes one reference of f on success. */
static int fd_install(struct pw_file *f)
{
	int fd = -1;

	AcquireSRWLockExclusive(&fd_lock);
	for (int i = 0; i < PANWIN_FD_MAX; i++) {
		if (!fds[i]) {
			fds[i] = f;
			fd = PANWIN_FD_BASE + i;
			break;
		}
	}
	ReleaseSRWLockExclusive(&fd_lock);
	if (fd < 0)
		errno = EMFILE;
	return fd;
}

/* ABI handshake: the kernel's GET_INFO must report this build's PANWIN_ABI_VERSION. */
static int check_abi(const struct panwin_transport *tp, void *tfile)
{
	struct panwin_info info;
	uint32_t returned = 0;
	int r;

	memset(&info, 0, sizeof(info));
	r = pw_ioctl_status_to_errno(tp->ioctl(tfile, PANWIN_IOCTL_GET_INFO, &info, sizeof(info),
					       sizeof(info), &returned));
	if (r)
		return r;
	if (returned < sizeof(info.abi_version) || info.abi_version != PANWIN_ABI_VERSION) {
		fprintf(stderr, "libdrm-win: panwin.sys ABI %u, this libdrm.dll needs ABI %u\n",
			info.abi_version, PANWIN_ABI_VERSION);
		return EPROTO;
	}
	return 0;
}

int panwin_open(const char *path, int flags)
{
	const struct panwin_transport *tp = pw_transport();
	struct pw_file *f;
	int index, type, r, fd;

	if (pw_parse_node(path, &index, &type)) {
		errno = ENOENT;
		return -1;
	}
	f = calloc(1, sizeof(*f));
	if (!f) {
		errno = ENOMEM;
		return -1;
	}
	r = tp->open(index, &f->tfile);
	if (r) {
		free(f);
		errno = -r;
		return -1;
	}
	if (!(flags & PANWIN_O_NOABICHECK)) {
		r = check_abi(tp, f->tfile);
		if (r) {
			tp->close(f->tfile);
			free(f);
			errno = r;
			return -1;
		}
	}
	f->tp = tp;
	f->index = index;
	f->refs = 1;
	AcquireSRWLockExclusive(&fd_lock);
	open_count++;
	ReleaseSRWLockExclusive(&fd_lock);

	fd = fd_install(f);
	if (fd < 0)
		pw_file_put(f);
	return fd;
}

int panwin_close(int fd)
{
	struct pw_file *f = NULL;
	unsigned slot = (unsigned)(fd - PANWIN_FD_BASE);

	if (slot < PANWIN_FD_MAX) {
		AcquireSRWLockExclusive(&fd_lock);
		f = fds[slot];
		fds[slot] = NULL;
		ReleaseSRWLockExclusive(&fd_lock);
	}
	if (!f) {
		errno = EBADF;
		return -1;
	}
	pw_file_put(f);
	return 0;
}

int panwin_dup(int fd)
{
	struct pw_file *f = pw_file_get(fd);
	int nfd;

	if (!f)
		return -1;
	nfd = fd_install(f); /* the reference from pw_file_get is transferred to the new fd */
	if (nfd < 0)
		pw_file_put(f);
	return nfd;
}

int panwin_is_fd(int fd)
{
	unsigned slot = (unsigned)(fd - PANWIN_FD_BASE);
	int r = 0;

	if (slot < PANWIN_FD_MAX) {
		AcquireSRWLockShared(&fd_lock);
		r = fds[slot] != NULL;
		ReleaseSRWLockShared(&fd_lock);
	}
	return r;
}

void *panwin_fd_to_handle(int fd)
{
	struct pw_file *f = pw_file_get(fd);
	void *h;

	if (!f)
		return NULL;
	h = f->tp->os_handle(f->tfile);
	pw_file_put(f);
	return h;
}

int pw_fd_ioctl(int fd, uint32_t code, void *buf, uint32_t in_size, uint32_t out_size)
{
	struct pw_file *f = pw_file_get(fd);
	uint32_t returned = 0;
	int r;

	if (!f)
		return -1;
	r = pw_ioctl_status_to_errno(f->tp->ioctl(f->tfile, code, buf, in_size, out_size, &returned));
	pw_file_put(f);
	if (r) {
		errno = r;
		return -1;
	}
	return 0;
}

int panwin_private_ioctl(int fd, uint32_t code, void *buf, uint32_t size)
{
	return pw_fd_ioctl(fd, code, buf, size, size);
}

int panwin_syncobj_event(int fd, uint32_t handle, uint64_t point, uint32_t flags, void **event)
{
	struct panwin_syncobj_event ev = { .handle = handle, .flags = flags, .point = point };
	HANDLE h;

	if (!event) {
		errno = EINVAL;
		return -1;
	}
	/* ABI v2: the caller creates the event; the kernel references it by handle
	 * (EVENT_MODIFY_STATE) and sets it once. Manual-reset, so it stays signalled. */
	h = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!h) {
		errno = panwin_win32_error_to_errno(GetLastError());
		return -1;
	}
	ev.event_handle = (uint64_t)(uintptr_t)h;
	if (pw_fd_ioctl(fd, PANWIN_IOCTL_SYNCOBJ_EVENT, &ev, sizeof(ev), 0)) {
		int e = errno;
		CloseHandle(h);
		errno = e;
		return -1;
	}
	*event = h;
	return 0;
}

/* ---- mmap emulation ---- */

struct pw_mapping {
	void *va;
	uint64_t size;
	struct pw_file *f; /* holds a reference */
};

static SRWLOCK map_lock = SRWLOCK_INIT;
static struct pw_mapping *maps;
static size_t map_count, map_cap;

static uint64_t page_size(void)
{
	static volatile LONG cached;
	if (!cached) {
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		InterlockedExchange(&cached, (LONG)si.dwPageSize);
	}
	return (uint64_t)cached;
}

void *panwin_mmap(int fd, uint64_t offset, size_t size, int prot)
{
	struct pw_file *f;
	struct panwin_mmap m;
	uint64_t ps = page_size();
	uint32_t returned = 0;
	int r;

	if (size == 0 || (offset & (ps - 1))) {
		errno = EINVAL;
		return NULL;
	}
	f = pw_file_get(fd);
	if (!f)
		return NULL;

	memset(&m, 0, sizeof(m));
	m.offset = offset;
	m.size = ((uint64_t)size + ps - 1) & ~(ps - 1);
	m.prot = (uint32_t)prot & (PANWIN_PROT_READ | PANWIN_PROT_WRITE);
	r = pw_ioctl_status_to_errno(f->tp->ioctl(f->tfile, PANWIN_IOCTL_MMAP, &m, sizeof(m),
						  sizeof(m), &returned));
	if (r || returned != sizeof(m) || !m.user_va) {
		pw_file_put(f);
		errno = r ? r : EIO;
		return NULL;
	}

	AcquireSRWLockExclusive(&map_lock);
	if (map_count == map_cap) {
		size_t ncap = map_cap ? map_cap * 2 : 64;
		struct pw_mapping *n = realloc(maps, ncap * sizeof(*n));
		if (!n) {
			ReleaseSRWLockExclusive(&map_lock);
			struct panwin_munmap um = { .user_va = m.user_va, .size = m.size };
			f->tp->ioctl(f->tfile, PANWIN_IOCTL_MUNMAP, &um, sizeof(um), 0, &returned);
			pw_file_put(f);
			errno = ENOMEM;
			return NULL;
		}
		maps = n;
		map_cap = ncap;
	}
	maps[map_count].va = (void *)(uintptr_t)m.user_va;
	maps[map_count].size = m.size;
	maps[map_count].f = f; /* keeps the file alive, as a Linux mapping keeps its struct file */
	map_count++;
	ReleaseSRWLockExclusive(&map_lock);

	return (void *)(uintptr_t)m.user_va;
}

int panwin_munmap(void *ptr, size_t size)
{
	struct pw_mapping mp = { 0 };
	bool found = false;
	uint32_t returned = 0;
	int r;

	(void)size; /* whole mapping is always released; partial munmap is not supported */
	AcquireSRWLockExclusive(&map_lock);
	for (size_t i = 0; i < map_count; i++) {
		if (maps[i].va == ptr) {
			mp = maps[i];
			maps[i] = maps[--map_count];
			found = true;
			break;
		}
	}
	ReleaseSRWLockExclusive(&map_lock);
	if (!found) {
		errno = EINVAL;
		return -1;
	}

	struct panwin_munmap um = { .user_va = (uint64_t)(uintptr_t)mp.va, .size = mp.size };
	r = pw_ioctl_status_to_errno(mp.f->tp->ioctl(mp.f->tfile, PANWIN_IOCTL_MUNMAP, &um,
						     sizeof(um), 0, &returned));
	pw_file_put(mp.f);
	if (r) {
		errno = r;
		return -1;
	}
	return 0;
}

int panwin_is_mapping(const void *ptr)
{
	int r = 0;

	AcquireSRWLockShared(&map_lock);
	for (size_t i = 0; i < map_count; i++) {
		if (maps[i].va == ptr) {
			r = 1;
			break;
		}
	}
	ReleaseSRWLockShared(&map_lock);
	return r;
}
