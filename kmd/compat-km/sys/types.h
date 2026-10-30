/* SPDX-License-Identifier: MIT */
/*
 * Minimal <sys/types.h> for freestanding Windows kernel builds of kmd/core.
 *
 * Mesa's drm.h takes its non-Linux path on Windows and includes <stdint.h>, <sys/types.h> and
 * <sys/ioccom.h>. The only thing it needs from <sys/types.h> is size_t (for __kernel_size_t),
 * which <stddef.h> (a freestanding header) provides. Shadowing the mingw CRT header keeps CRT
 * headers out of the kernel build.
 */
#ifndef PANWIN_COMPAT_KM_SYS_TYPES_H
#define PANWIN_COMPAT_KM_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#endif
