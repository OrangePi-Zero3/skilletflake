// SPDX-License-Identifier: MIT
/* pw_util.c - freestanding string/memory helpers and a small snprintf for the Mali core. */

#include "pw_util.h"

void *pw_memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *pw_memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;

	while (n--)
		*d++ = (uint8_t)c;
	return dst;
}

int pw_memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (; n; n--, x++, y++) {
		if (*x != *y)
			return *x < *y ? -1 : 1;
	}
	return 0;
}

size_t pw_strlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

size_t pw_strnlen(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n])
		n++;
	return n;
}

size_t pw_strlcpy(char *dst, const char *src, size_t size)
{
	size_t len = pw_strlen(src);

	if (size) {
		size_t n = len < size - 1 ? len : size - 1;

		pw_memcpy(dst, src, n);
		dst[n] = '\0';
	}
	return len;
}

struct pw_fmt_out {
	char *buf;
	size_t size;
	size_t pos;
};

static void out_c(struct pw_fmt_out *o, char c)
{
	if (o->size && o->pos < o->size - 1)
		o->buf[o->pos] = c;
	o->pos++;
}

static void out_pad(struct pw_fmt_out *o, char c, int n)
{
	while (n-- > 0)
		out_c(o, c);
}

static void out_str(struct pw_fmt_out *o, const char *s, int width, int prec, bool left)
{
	size_t len;

	if (!s)
		s = "(null)";
	len = prec >= 0 ? pw_strnlen(s, (size_t)prec) : pw_strlen(s);
	if (!left)
		out_pad(o, ' ', width - (int)len);
	for (size_t i = 0; i < len; i++)
		out_c(o, s[i]);
	if (left)
		out_pad(o, ' ', width - (int)len);
}

static void out_num(struct pw_fmt_out *o, unsigned long long v, bool neg, unsigned int base,
		    bool upper, int width, bool zero, bool left)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char tmp[24];
	int n = 0, len;

	do {
		tmp[n++] = digits[v % base];
		v /= base;
	} while (v);

	len = n + (neg ? 1 : 0);
	if (!left && !zero)
		out_pad(o, ' ', width - len);
	if (neg)
		out_c(o, '-');
	if (!left && zero)
		out_pad(o, '0', width - len);
	while (n)
		out_c(o, tmp[--n]);
	if (left)
		out_pad(o, ' ', width - len);
}

int pw_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct pw_fmt_out o = { buf, size, 0 };

	while (*fmt) {
		bool left = false, zero = false;
		int width = 0, prec = -1, lng = 0;
		char c = *fmt++;

		if (c != '%') {
			out_c(&o, c);
			continue;
		}

		for (;; fmt++) {
			if (*fmt == '-')
				left = true;
			else if (*fmt == '0')
				zero = true;
			else
				break;
		}
		if (*fmt == '*') {
			width = va_arg(ap, int);
			fmt++;
		} else {
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');
		}
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			} else {
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec * 10 + (*fmt++ - '0');
			}
		}
		/* lng: -2 hh, -1 h, 0 int, 1 long, 2 long long, 3 size_t */
		if (*fmt == 'h') {
			fmt++;
			lng = -1;
			if (*fmt == 'h') {
				fmt++;
				lng = -2;
			}
		} else if (*fmt == 'l') {
			fmt++;
			lng = 1;
			if (*fmt == 'l') {
				fmt++;
				lng = 2;
			}
		} else if (*fmt == 'z') {
			fmt++;
			lng = 3;
		}

		c = *fmt;
		if (!c)
			break;
		fmt++;

		switch (c) {
		case 'd':
		case 'i': {
			long long v;

			if (lng == 2)
				v = va_arg(ap, long long);
			else if (lng == 1)
				v = va_arg(ap, long);
			else if (lng == 3)
				v = (long long)va_arg(ap, size_t);
			else
				v = va_arg(ap, int);
			if (lng == -1)
				v = (short)v;
			else if (lng == -2)
				v = (signed char)v;
			out_num(&o, v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v,
				v < 0, 10, false, width, zero, left);
			break;
		}
		case 'u':
		case 'x':
		case 'X': {
			unsigned long long v;

			if (lng == 2)
				v = va_arg(ap, unsigned long long);
			else if (lng == 1)
				v = va_arg(ap, unsigned long);
			else if (lng == 3)
				v = va_arg(ap, size_t);
			else
				v = va_arg(ap, unsigned int);
			if (lng == -1)
				v = (unsigned short)v;
			else if (lng == -2)
				v = (unsigned char)v;
			out_num(&o, v, false, c == 'u' ? 10 : 16, c == 'X', width, zero, left);
			break;
		}
		case 'p':
			out_c(&o, '0');
			out_c(&o, 'x');
			out_num(&o, (unsigned long long)(uintptr_t)va_arg(ap, void *), false, 16,
				false, width > 2 ? width - 2 : 0, true, false);
			break;
		case 'c':
			out_c(&o, (char)va_arg(ap, int));
			break;
		case 's':
			out_str(&o, va_arg(ap, const char *), width, prec, left);
			break;
		case '%':
			out_c(&o, '%');
			break;
		default:
			/* Unknown conversion: print it verbatim. */
			out_c(&o, '%');
			out_c(&o, c);
			break;
		}
	}

	if (size)
		buf[o.pos < size ? o.pos : size - 1] = '\0';
	return o.pos > 0x7fffffff ? 0x7fffffff : (int)o.pos;
}

int pw_snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = pw_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return r;
}
