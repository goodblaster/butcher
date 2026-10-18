/**
 * @file errbuf.cpp
 *
 * See errbuf.h.
 */
#include "errbuf.h"

#include <stdarg.h>
#include <stdio.h>

void errbuf_set(char *err, size_t cap, const char *fmt, ...)
{
	if (err == NULL || cap == 0)
		return;
	va_list va;
	va_start(va, fmt);
	vsnprintf(err, cap, fmt, va);
	va_end(va);
}
