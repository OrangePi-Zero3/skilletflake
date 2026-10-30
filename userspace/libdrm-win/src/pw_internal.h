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

/* Transports: see include/panwin_transport.h (the public form of this table). */
#include "panwin_transport.h"

extern const struct panwin_transport pw_transport_win32;
#ifdef PANWIN_HAVE_FAKE_KERNEL
extern const struct panwin_transport pw_transport_fake;
#endif
#ifdef PANWIN_HAVE_CORE_TRANSPORT
/* transport_core.c: the real kmd/core on the mock G610, in-process (test builds). */
extern const struct panwin_transport pw_transport_core;
#endif

/* The transport in use (selected at first use, see panwin_transport.h). */
const struct panwin_transport *pw_transport(void);

/* NTSTATUS returned by a transport ioctl -> 0 or a positive CRT errno. Success-class codes are
 * success except STATUS_TIMEOUT (ETIME) and STATUS_USER_APC / STATUS_ALERTED (EINTR). */
int pw_ioctl_status_to_errno(int32_t status);

/* Emulated DRM file (shared by dup'ed fds and by mappings). */
struct pw_file {
	volatile long refs;
	const struct panwin_transport *tp;
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
