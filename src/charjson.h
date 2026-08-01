/**
 * @file charjson.h
 *
 * Text interchange format for a character: PkPlayerStruct <-> JSON.
 *
 * The guarantee this file exists to provide is **losslessness**. Every one of
 * the 1266 bytes must survive export followed by import, because anything less
 * means a user who exports, edits one number, and imports has silently damaged
 * the rest of their character. That is why fields nobody wants to look at --
 * bReserved, dest_action, archive_time -- are still in the document. They are
 * not always zero: real Hellfire saves carry bReserved[1] == 1.
 *
 * Two consequences worth knowing:
 *
 *  - Life and mana are fixed point with 6 fractional bits, and real saves do
 *    carry fractions (a Monk at 54 life is stored as 3458 = 54 + 2/64). The
 *    whole number is the editable field; the remainder is preserved beside it
 *    and only written when nonzero.
 *
 *  - The one thing NOT preserved is nonzero padding after the NUL in pName.
 *    The game always zero-pads, so this cannot arise from a real save; export
 *    warns if it ever sees it.
 */
#ifndef BUTCHER_CHARJSON_H
#define BUTCHER_CHARJSON_H

#include "diag.h"
#include "hero.h"
#include "json.h" /* JSON_ERR_LEN, for callers sizing their error buffers */

#define BUTCHER_FORMAT_VERSION 1

/**
 * Render a character as JSON.
 * @param warn called for anything the document cannot represent; may be NULL.
 * @return malloc'd NUL-terminated text the caller frees.
 */
char *charjson_write(const PkPlayerStruct *h, HeroFlavor f,
    void (*warn)(const char *));

/**
 * Parse a character from JSON into @p out.
 *
 * Every field defaults to the zero value when absent, so a partial document
 * describes a mostly-blank character rather than failing. To edit one field of
 * an existing character, export it first and edit that -- or use `set`.
 *
 * @param flavor receives the game named in the document, if any.
 * @return nonzero on success; err holds a line and column on failure.
 */
int charjson_read(const char *text, PkPlayerStruct *out, HeroFlavor *flavor,
    char *err);

/**
 * Check a document without applying it: syntax, unknown or misspelled keys,
 * wrong types, and values that would not survive the field they are stored in.
 *
 * These are the failures a hand-edited file actually suffers. A misspelled key
 * is the worst of them: the value is simply ignored and the real field imports
 * as zero, which looks like a successful import.
 *
 * Findings are appended to @p dl; nothing is printed.
 */
void charjson_check(const char *text, DiagList *dl);

/** Names of the seven equipment slots, in InvBody order. */
extern const char *const charjson_equip_slots[NUM_INVLOC];

#endif /* BUTCHER_CHARJSON_H */
