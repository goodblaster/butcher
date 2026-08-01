/**
 * @file format.h
 *
 * Human-readable rendering of a character.
 */
#ifndef BUTCHER_FORMAT_H
#define BUTCHER_FORMAT_H

#include "hero.h"

/** Base item name for a packed IDidx, or NULL if out of range. */
const char *format_item_name(HeroFlavor f, int idx);

/** One-line summary, for `list`. */
void format_brief(const PkPlayerStruct *h, HeroFlavor f, FILE *out);

/** Full character sheet, for `show`. */
void format_show(const PkPlayerStruct *h, HeroFlavor f, FILE *out);

/** Print field-by-field differences between two characters. @return count. */
int format_diff(const PkPlayerStruct *before, const PkPlayerStruct *after,
    HeroFlavor f, FILE *out);

#endif /* BUTCHER_FORMAT_H */
