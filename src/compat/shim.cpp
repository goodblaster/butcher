/**
 * @file shim.cpp
 *
 * Definitions backing compat/shim.h.
 */
#include "shim.h"

#include <stdarg.h>

/*
 * msvcrt seeds its PRNG to 1 before any srand() call. codec_init_key always
 * calls srand(0x7058) first, so this initial value never affects save
 * decoding -- it exists so the published-sequence test in crypto_check.cpp
 * can exercise the default-seed behaviour.
 */
uint32_t dvl_msvc_rand_seed = 1;

void app_fatal(const char *pszFmt, ...)
{
	va_list va;
	va_start(va, pszFmt);
	fputs("fatal: ", stderr);
	vfprintf(stderr, pszFmt, va);
	fputc('\n', stderr);
	va_end(va);
	abort();
}

/*
 * The game's allocator (engine.cpp) aborts through app_fatal on exhaustion
 * rather than returning NULL, and Source/encrypt.cpp relies on that -- it
 * never null-checks. Match the contract.
 */
BYTE *DiabloAllocPtr(DWORD dwBytes)
{
	BYTE *p = (BYTE *)malloc(dwBytes);
	if (p == NULL)
		app_fatal("out of memory allocating %u bytes", (unsigned)dwBytes);
	return p;
}

void mem_free_dbg(void *p)
{
	free(p);
}
