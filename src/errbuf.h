/**
 * @file errbuf.h
 *
 * The one way the library reports a failure: a message formatted into a
 * fixed-size buffer the caller owns, or nowhere when the caller passed NULL.
 *
 * Shared rather than copied into every file, and declared with the printf
 * format attribute so a mismatched argument is a compile-time warning instead
 * of a garbled message on the one path nobody tested.
 */
#ifndef BUTCHER_ERRBUF_H
#define BUTCHER_ERRBUF_H

#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#define ERRBUF_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define ERRBUF_PRINTF(fmt, args)
#endif

/** Format into @p err, which holds @p cap bytes. A NULL @p err is ignored. */
void errbuf_set(char *err, size_t cap, const char *fmt, ...) ERRBUF_PRINTF(3, 4);

#endif /* BUTCHER_ERRBUF_H */
