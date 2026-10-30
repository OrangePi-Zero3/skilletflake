// SPDX-License-Identifier: MIT
/*
 * pw_fmt.c - tiny freestanding vsnprintf (see pw_fmt.h for the supported subset).
 */
#include "pw_fmt.h"

#include <stdint.h>

struct pw_fmt_out {
	char *buf;
	size_t size;
	size_t pos; /* chars produced so far (may exceed size) */
};

static void out_c(struct pw_fmt_out *o, char c)
{
	if (o->pos + 1 < o->size)
		o->buf[o->pos] = c;
	o->pos++;
}

static void out_pad(struct pw_fmt_out *o, char c, int n)
{
	while (n-- > 0)
		out_c(o, c);
}

enum {
	F_LEFT = 1 << 0,
	F_PLUS = 1 << 1,
	F_SPACE = 1 << 2,
	F_ALT = 1 << 3,
	F_ZERO = 1 << 4,
};

static size_t bounded_strlen(const char *s, int prec)
{
	size_t n = 0;

	while ((prec < 0 || n < (size_t)prec) && s[n])
		n++;
	return n;
}

static void out_str(struct pw_fmt_out *o, const char *s, int flags, int width, int prec)
{
	size_t len = bounded_strlen(s, prec);
	int pad = width > (int)len ? width - (int)len : 0;

	if (!(flags & F_LEFT))
		out_pad(o, ' ', pad);
	for (size_t i = 0; i < len; i++)
		out_c(o, s[i]);
	if (flags & F_LEFT)
		out_pad(o, ' ', pad);
}

static void out_num(struct pw_fmt_out *o, uintmax_t v, int neg, unsigned int base, int upper,
		    int flags, int width, int prec, const char *prefix)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char tmp[24]; /* 2^64 in octal is 22 digits */
	int n = 0, zeros, pad, plen = 0;
	char sign = 0;

	if (!(v == 0 && prec == 0)) {
		do {
			tmp[n++] = digits[v % base];
			v /= base;
		} while (v);
	}
	if (neg)
		sign = '-';
	else if (flags & F_PLUS)
		sign = '+';
	else if (flags & F_SPACE)
		sign = ' ';

	if (prefix)
		while (prefix[plen])
			plen++;
	/* "%#o": the precision is raised so that the first digit is 0. */
	if (base == 8 && (flags & F_ALT) && (n == 0 || tmp[n - 1] != '0') && prec <= n)
		prec = n + 1;

	zeros = prec > n ? prec - n : 0;
	pad = width - (n + zeros + (sign ? 1 : 0) + plen);
	if (pad < 0)
		pad = 0;
	if ((flags & F_ZERO) && !(flags & F_LEFT) && prec < 0) {
		zeros += pad;
		pad = 0;
	}

	if (!(flags & F_LEFT))
		out_pad(o, ' ', pad);
	if (sign)
		out_c(o, sign);
	for (int i = 0; i < plen; i++)
		out_c(o, prefix[i]);
	out_pad(o, '0', zeros);
	while (n)
		out_c(o, tmp[--n]);
	if (flags & F_LEFT)
		out_pad(o, ' ', pad);
}

enum len_mod { L_NONE, L_HH, L_H, L_L, L_LL, L_Z, L_J, L_T };

static intmax_t arg_signed(va_list *ap, enum len_mod l)
{
	switch (l) {
	case L_HH: return (signed char)va_arg(*ap, int);
	case L_H:  return (short)va_arg(*ap, int);
	case L_L:  return va_arg(*ap, long);
	case L_LL: return va_arg(*ap, long long);
	case L_Z:  return (intmax_t)(intptr_t)va_arg(*ap, size_t);
	case L_J:  return va_arg(*ap, intmax_t);
	case L_T:  return va_arg(*ap, ptrdiff_t);
	default:   return va_arg(*ap, int);
	}
}

static uintmax_t arg_unsigned(va_list *ap, enum len_mod l)
{
	switch (l) {
	case L_HH: return (unsigned char)va_arg(*ap, unsigned int);
	case L_H:  return (unsigned short)va_arg(*ap, unsigned int);
	case L_L:  return va_arg(*ap, unsigned long);
	case L_LL: return va_arg(*ap, unsigned long long);
	case L_Z:  return va_arg(*ap, size_t);
	case L_J:  return va_arg(*ap, uintmax_t);
	case L_T:  return (uintmax_t)(uintptr_t)va_arg(*ap, ptrdiff_t);
	default:   return va_arg(*ap, unsigned int);
	}
}

static int parse_int(const char **pp)
{
	int v = 0;

	while (**pp >= '0' && **pp <= '9') {
		if (v < 100000)
			v = v * 10 + (**pp - '0');
		(*pp)++;
	}
	return v;
}

int pw_win_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap_in)
{
	struct pw_fmt_out o = { .buf = buf, .size = size, .pos = 0 };
	va_list ap;
	const char *p = fmt;

	va_copy(ap, ap_in);
	while (*p) {
		const char *start = p;
		int flags = 0, width = 0, prec = -1;
		enum len_mod l = L_NONE;

		if (*p != '%') {
			out_c(&o, *p++);
			continue;
		}
		p++;
		for (;; p++) {
			if (*p == '-') flags |= F_LEFT;
			else if (*p == '+') flags |= F_PLUS;
			else if (*p == ' ') flags |= F_SPACE;
			else if (*p == '#') flags |= F_ALT;
			else if (*p == '0') flags |= F_ZERO;
			else break;
		}
		if (*p == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				flags |= F_LEFT;
				width = width < -0x7fffffff ? 0 : -width;
			}
			p++;
		} else {
			width = parse_int(&p);
		}
		if (*p == '.') {
			p++;
			if (*p == '*') {
				prec = va_arg(ap, int);
				if (prec < 0)
					prec = -1;
				p++;
			} else {
				prec = parse_int(&p);
			}
		}
		switch (*p) {
		case 'h':
			l = L_H;
			if (p[1] == 'h') { l = L_HH; p++; }
			p++;
			break;
		case 'l':
			l = L_L;
			if (p[1] == 'l') { l = L_LL; p++; }
			p++;
			break;
		case 'z': l = L_Z; p++; break;
		case 'j': l = L_J; p++; break;
		case 't': l = L_T; p++; break;
		default: break;
		}

		switch (*p) {
		case 'd':
		case 'i': {
			intmax_t v = arg_signed(&ap, l);
			uintmax_t mag = v < 0 ? (uintmax_t)0 - (uintmax_t)v : (uintmax_t)v;

			out_num(&o, mag, v < 0, 10, 0, flags, width, prec, NULL);
			break;
		}
		case 'u':
			out_num(&o, arg_unsigned(&ap, l), 0, 10, 0, flags & ~(F_PLUS | F_SPACE),
				width, prec, NULL);
			break;
		case 'o':
			out_num(&o, arg_unsigned(&ap, l), 0, 8, 0, flags & ~(F_PLUS | F_SPACE),
				width, prec, NULL);
			break;
		case 'x':
		case 'X': {
			uintmax_t v = arg_unsigned(&ap, l);
			const char *pfx = (flags & F_ALT) && v ? (*p == 'X' ? "0X" : "0x") : NULL;

			out_num(&o, v, 0, 16, *p == 'X', flags & ~(F_PLUS | F_SPACE), width, prec, pfx);
			break;
		}
		case 'p': {
			void *v = va_arg(ap, void *);

			if (!v)
				out_str(&o, "(nil)", flags, width, -1);
			else
				out_num(&o, (uintptr_t)v, 0, 16, 0, flags & ~F_ZERO, width, prec, "0x");
			break;
		}
		case 'c': {
			char c = (char)va_arg(ap, int);
			int pad = width > 1 ? width - 1 : 0;

			if (!(flags & F_LEFT))
				out_pad(&o, ' ', pad);
			out_c(&o, c);
			if (flags & F_LEFT)
				out_pad(&o, ' ', pad);
			break;
		}
		case 's': {
			const char *s = va_arg(ap, const char *);

			out_str(&o, s ? s : "(null)", flags, width, prec);
			break;
		}
		case '%':
			out_c(&o, '%');
			break;
		default:
			/* Unsupported/unknown conversion (or end of string): copy it verbatim. */
			while (start < p)
				out_c(&o, *start++);
			if (*p)
				out_c(&o, *p);
			else
				p--;
			break;
		}
		p++;
	}
	va_end(ap);

	if (size)
		buf[o.pos < size ? o.pos : size - 1] = '\0';
	return o.pos > 0x7fffffff ? 0x7fffffff : (int)o.pos;
}

int pw_win_snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = pw_win_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}
