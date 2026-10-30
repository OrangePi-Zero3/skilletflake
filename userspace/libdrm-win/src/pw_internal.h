/* SPDX-License-Identifier: MIT */
/* libdrm-win internals: transport abstraction, emulated files, helpers. */
#ifndef PW_INTERNAL_H
#define PW_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define PW_MAX_DEVICES 16
#define PW_RENDER_MINOR_BASE 128
#define PW_PRIMARY_MINOR_BASE 0

/*
 * A transport turns "device index i" into a kernel file and carries ioctls to it. The real one
 * uses SetupAPI + CreateFileW + NtDeviceIoControlFile; the fake one (tests) emulates panwin.sys
 * in-process. All entry points return 0 or a negative errno.
 */
struct pw_transport {
	const char *name;
	/* Number of devices present (<= PW_MAX_DEVICES), or negative errno. */
	int (*enumerate)(void);
	/* Human-readable kernel path of device `index` (e.g. \\?\ACPI#RKCP7410#...), for diagnostics. */
	int (*device_path)(int index, char *buf, size_t len);
	int (*open)(int index, void **file);
	void (*close)(void *file);
	/* METHOD_BUFFERED semantics: `buf` holds in_size input bytes and receives up to out_size
	 * output bytes; *returned = bytes written back. */
	int (*ioctl)(void *file, uint32_t code, void *buf, uint32_t in_size, uint32_t out_size,
		     uint32_t *returned);
	/* Value returned by panwin_fd_to_handle(). */
	void *(*os_handle)(void *file);
};

extern const struct pw_transport pw_transport_win32;
#ifdef PANWIN_HAVE_FAKE_KERNEL
extern const struct pw_transport pw_transport_fake;
#endif

const struct pw_transport *pw_transport(void);

/* Emulated DRM file (shared by dup'ed fds and by mappings). */
struct pw_file {
	volatile long refs;
	const struct pw_transport *tp;
	void *tfile;
	int index;      /* device index */
};

/* fd table (fd.c). pw_file_get() takes a reference (release with pw_file_put), NULL + errno=EBADF. */
struct pw_file *pw_file_get(int fd);
void pw_file_put(struct pw_file *f);

/* Issue a raw ioctl on an fd: returns 0 or -1 with errno set. */
int pw_fd_ioctl(int fd, uint32_t code, void *buf, uint32_t in_size, uint32_t out_size);

/* Node path helpers (device.c). */
int pw_parse_node(const char *path, int *index, int *node_type);
void pw_format_node(char *buf, size_t len, int index, int node_type);

#define PW_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#endif /* PW_INTERNAL_H */
