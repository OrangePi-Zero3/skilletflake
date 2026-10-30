/* SPDX-License-Identifier: MIT */
/*
 * pw_fmt.h - tiny freestanding vsnprintf used by the Windows kernel glue for pw_os_log().
 *
 * Why not RtlStringCbVPrintfA / DbgPrintEx's own formatter: ntstrsafe's inline wrappers call
 * ntoskrnl's _vsnprintf, whose handling of C99 length modifiers (%zu, %hhx, %jd, %td) is not
 * documented, and the core (ported from Linux) formats with standard C99 conversions. This
 * formatter implements exactly C99 semantics for the subset below, so the same core format
 * strings print the same on the Linux host tests and on Windows.
 *
 * Supported: flags "-+ #0", width and precision (including '*'), length hh h l ll z j t,
 * conversions d i u o x X c s p %. No floating point, no %n. %p prints like glibc
 * ("0x1f" / "(nil)"), %s of NULL prints "(null)". Unknown conversions are copied verbatim.
 * No OS headers: the file is also built and tested on the Linux host (kmd/win/tests).
 */
#ifndef PW_FMT_H
#define PW_FMT_H

#include <stdarg.h>
#include <stddef.h>

/* C99 vsnprintf semantics: writes at most size-1 chars + NUL (if size > 0) and returns the
 * length the full output would have had. */
int pw_win_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int pw_win_snprintf(char *buf, size_t size, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((format(printf, 3, 4)))
#endif
	;

#endif /* PW_FMT_H */
