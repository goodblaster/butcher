/**
 * @file shim.h
 *
 * Portable compilation shim for reusing Devilution's save-file crypto
 * (Source/sha.cpp, Source/codec.cpp) on a non-Windows host.
 *
 * Nothing under Source/ may be edited -- it is a byte-accuracy
 * reconstruction. This header makes those files compile unmodified by:
 *
 *   1. Pre-defining __ALL_H__ so that the `#include "all.h"` at the top of
 *      each one expands to nothing. Quoted includes resolve relative to the
 *      including file, so Source/all.h cannot be shadowed via -I; tripping
 *      its own include guard is the only way to neutralize it that does not
 *      involve touching the file.
 *   2. Supplying the handful of Win32 types the root headers need, then
 *      including the real defs.h / enums.h / structs.h so that struct
 *      layouts come from the authoritative source rather than a copy.
 *   3. Replacing rand()/srand() with the msvcrt LCG. See the comment on
 *      dvl_msvc_rand -- this is load-bearing, not a portability nicety.
 *
 * Force-included via -include; see the Makefile.
 */
#ifndef BUTCHER_SHIM_H
#define BUTCHER_SHIM_H

/* Neutralize Source/all.h. Must precede any include of a Source .cpp file. */
#define __ALL_H__

#include <ctype.h> /* Source/encrypt.cpp: toupper, normally via types.h */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Compile-time assertion. Layout facts the tool depends on are pinned with
 * this so a change to defs.h / structs.h breaks the build instead of
 * silently corrupting saves.
 */
#define CT_ASSERT(cond, tag) typedef char butcher_ct_##tag[(cond) ? 1 : -1]

/* ------------------------------------------------------------------ */
/* Win32 types                                                         */
/* ------------------------------------------------------------------ */

typedef uint8_t BYTE;
typedef uint8_t UCHAR;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int BOOL;
typedef unsigned char BOOLEAN;

/* Calling-convention keywords are meaningless on the host target. */
#define __stdcall
#define __fastcall
#define __cdecl

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

/* structs.h spells 64-bit integers the MSVC way. */
#define __int64 long long

typedef struct FILETIME {
	DWORD dwLowDateTime;
	DWORD dwHighDateTime;
} FILETIME;

/*
 * Opaque stand-ins. These appear only in structs the editor never touches
 * (TSnd, _SNETUIDATA, MEMFILE). Their width differs from Win32 on a 64-bit
 * host, which is harmless here: every struct this tool actually reads or
 * writes lives inside a #pragma pack(1) region and contains no pointers.
 * Do not add a size assertion for any struct containing one of these.
 */
typedef void *HWND;
typedef void *HANDLE;
typedef void *LPDIRECTSOUNDBUFFER;

typedef struct WAVEFORMATEX {
	WORD wFormatTag;
	WORD nChannels;
	DWORD nSamplesPerSec;
	DWORD nAvgBytesPerSec;
	WORD nBlockAlign;
	WORD wBitsPerSample;
	WORD cbSize;
} WAVEFORMATEX;

/* ------------------------------------------------------------------ */
/* Game headers, verbatim                                              */
/* ------------------------------------------------------------------ */

#include "../../third_party/devilution/defs.h"
#include "../../third_party/devilution/enums.h"
#include "../../third_party/devilution/structs.h"

#include "../../third_party/devilution/Source/sha.h"
#include "../../third_party/devilution/Source/codec.h"

/* ------------------------------------------------------------------ */
/* msvcrt rand                                                         */
/* ------------------------------------------------------------------ */

/*
 * codec_init_key() seeds srand(0x7058) and fills its 136-byte key buffer
 * from rand(). The keystream therefore depends on the C runtime's PRNG,
 * not on anything in this repository. MSVC and MinGW both use the msvcrt
 * LCG below; glibc and Apple libc use entirely different generators, so a
 * host-native build calling the system rand() derives a different key and
 * fails every checksum.
 *
 * Reproduces msvcrt exactly:  seed = seed * 214013 + 2531011
 *                             return (seed >> 16) & 0x7fff
 * Verified against the published srand(1) sequence in tests/crypto.cpp.
 */
extern uint32_t dvl_msvc_rand_seed;

static inline void dvl_msvc_srand(unsigned int seed)
{
	dvl_msvc_rand_seed = (uint32_t)seed;
}

static inline int dvl_msvc_rand(void)
{
	dvl_msvc_rand_seed = dvl_msvc_rand_seed * 214013u + 2531011u;
	return (int)((dvl_msvc_rand_seed >> 16) & 0x7fff);
}

#define rand dvl_msvc_rand
#define srand dvl_msvc_srand

/* ------------------------------------------------------------------ */
/* Fatal handler                                                       */
/* ------------------------------------------------------------------ */

/*
 * codec_encode() calls app_fatal on bad parameters. The game pops a dialog;
 * here it must be loud and non-returning so a bug cannot be mistaken for a
 * corrupt save.
 */
void app_fatal(const char *pszFmt, ...);

/* Source/encrypt.cpp allocates through these (normally engine.cpp). */
BYTE *DiabloAllocPtr(DWORD dwBytes);
void mem_free_dbg(void *p);

#endif /* BUTCHER_SHIM_H */
