/**
 * @file savefile.h
 *
 * Access to Diablo save archives (single_N.sv).
 *
 * Devilution's MPQ layout is fixed and self-imposed (Source/mpqapi.cpp):
 * header at 0, block table at 104, hash table at 32872, data from 0x10068,
 * 2048 entries in each table, 4096-byte sectors. Anything else is rejected.
 *
 * IMPORTANT: ParseMPQHeader() in the game responds to a layout mismatch by
 * truncating the file to zero and starting a fresh archive. That is faithful
 * to the original and catastrophic in an editor. Nothing here ever truncates:
 * reads open the file "rb", and writes go to a temporary file that is
 * re-opened and verified before an atomic rename over the original.
 */
#ifndef BUTCHER_SAVEFILE_H
#define BUTCHER_SAVEFILE_H

#include "compat/shim.h"

/** Size of the caller-provided error buffer passed to every entry point. */
#define MPQ_ERR_LEN 256

/* Layout constants, mirrored from Source/mpqapi.cpp. */
#define MPQ_INDEX_ENTRIES 2048
#define MPQ_BLOCK_OFFSET 104
#define MPQ_HASH_OFFSET 32872
#define MPQ_DATA_OFFSET 0x10068 /* 32872 + 32768 */
#define MPQ_SECTOR_SIZE 4096
#define MPQ_HEADER_SIZE 104

/** 'MPQ\x1A' -- what MSVC's '\x1AQPM' multi-char literal evaluates to. */
#define MPQ_SIGNATURE 0x1A51504Du

/* Block flags. mpqapi always writes EXISTS|IMPLODE (0x80000100). */
#define MPQ_FLAG_IMPLODE 0x00000100u
#define MPQ_FLAG_COMPRESS 0x00000200u
#define MPQ_FLAG_ENCRYPTED 0x00010000u
#define MPQ_FLAG_SINGLEUNIT 0x01000000u
#define MPQ_FLAG_EXISTS 0x80000000u

/* Save-file passwords (Source/pfile.cpp). Spawn builds differ. */
#define SAVE_PASSWORD_SINGLE "xrgyrkj1"
#define SAVE_PASSWORD_MULTI "szqnlsk1"

typedef struct MpqArchive MpqArchive;

/**
 * Open an archive read-only and load its hash and block tables.
 * @return NULL on failure, with a description written to err.
 */
MpqArchive *mpq_open(const char *path, char *err);

void mpq_close(MpqArchive *a);

/** @return nonzero if a live hash entry exists for pszName. */
int mpq_has_file(MpqArchive *a, const char *name);

/**
 * Read and decompress one file.
 * @param out_len receives the uncompressed length
 * @return malloc'd buffer the caller frees, or NULL with err set.
 */
BYTE *mpq_read_file(MpqArchive *a, const char *name, DWORD *out_len, char *err);

/**
 * Read `hero` from a save and decode it into a PkPlayerStruct.
 * Tries the single-player password, then the multiplayer one.
 *
 * @param used_multi optional; set to 1 if the multiplayer password was the
 *        one that worked. save_write_hero only ever writes single-player
 *        encoding, so a caller intending to write back must check this.
 * @return nonzero on success.
 */
int save_read_hero(const char *path, PkPlayerStruct *out, int *used_multi, char *err);

/**
 * Replace one existing file inside an archive.
 *
 * The archive is rebuilt into a temporary file and renamed over the original
 * only after the result has been re-opened and verified. Files other than
 * @p name are copied as raw compressed bytes -- never decompressed and
 * recompressed -- so nothing can be lost or altered by a codec round-trip.
 * The hash table is preserved slot for slot, including deleted entries, so
 * probe chains stay intact; only block indices are remapped.
 *
 * Does not create new files: @p name must already exist in the archive.
 *
 * @return nonzero on success. On failure the original is untouched.
 */
int mpq_replace_file(const char *path, const char *name, const BYTE *data,
    DWORD len, char *err);

/** Encode a PkPlayerStruct and write it back as `hero`. */
int save_write_hero(const char *path, const PkPlayerStruct *hero, char *err);

#endif /* BUTCHER_SAVEFILE_H */
