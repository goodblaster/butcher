/**
 * @file hero.h
 *
 * The editable view of a character. Everything that knows what a *valid*
 * character looks like lives here; savefile.{h,cpp} only moves bytes.
 *
 * Diablo and Hellfire share the packed save format exactly -- PkPlayerStruct
 * is 1266 bytes in both, with every field at the same offset -- so reading
 * and writing need no flavor awareness at all. What differs is the meaning:
 * Hellfire adds three classes with their own stat caps, ten more spells, and
 * eight more dungeon levels. Everything below that varies takes a flavor.
 */
#ifndef BUTCHER_HERO_H
#define BUTCHER_HERO_H

#include "diag.h"
#include "savefile.h"

#define HERO_ERR_LEN 256

/** HP and mana are fixed point with 6 fractional bits: 1 point == 64. */
#define HERO_FIXED_SHIFT 6
#define HERO_TO_WHOLE(v) ((v) >> HERO_FIXED_SHIFT)
#define HERO_FROM_WHOLE(v) ((v) << HERO_FIXED_SHIFT)

typedef enum HeroFlavor {
	FLAVOR_DIABLO = 0,
	FLAVOR_HELLFIRE = 1
} HeroFlavor;

/** Infer the flavor from a save's extension: .hsv is Hellfire, .sv is Diablo. */
HeroFlavor hero_flavor_for_path(const char *path);

const char *hero_flavor_name(HeroFlavor f);

/* ---- the game's own limits ---- */

/** Highest attainable level (MAXCHARLEVEL - 1). Same in both flavors. */
int hero_max_level(void);

int hero_num_classes(HeroFlavor f);

/** Maximum base value for one attribute, per class. attrib is ATTRIB_*. */
int hero_max_stat(HeroFlavor f, int pclass, int attrib);

/** Number of dungeon levels (NUMLEVELS). */
int hero_num_levels(HeroFlavor f);

/** Minimum experience required to be at @p level. Shared by both flavors. */
int hero_exp_for_level(int level);

/** Level the game would compute from @p exp. */
int hero_level_for_exp(int exp);

const char *hero_class_name(HeroFlavor f, int pclass);

/**
 * The highest base life a character of this class could legitimately reach:
 * creation value at the class's vitality cap, plus every level to 50.
 *
 * Derived from CreatePlayer and NextPlrLevel rather than chosen, so a slider
 * bounded by it shows a meaningful fill instead of a sliver. Returns whole
 * points, not the fixed-point form.
 */
int hero_max_life(HeroFlavor f, int pclass);

/** The same for mana. A Hellfire Barbarian gains none per level. */
int hero_max_mana(HeroFlavor f, int pclass);

/* ---- spells ---- */

/** Spell ids this flavor defines (MAX_SPELLS). */
int hero_spell_slots(HeroFlavor f);

/**
 * One past the highest spell id a save can actually carry.
 *
 * PackPlayer stores ids 0..36 in pSplLvl[37] and 37..46 in pSplLvl2[10]
 * (Source/pack.cpp). Hellfire's MAX_SPELLS is 52, so ids 47..51 exist in the
 * game but have nowhere to live in a save file and are silently lost.
 */
int hero_spell_persisted(void);

/** @return spell id for a name (case- and space-insensitive), or -1. */
int hero_find_spell(HeroFlavor f, const char *name);

/** @return display name for a spell id, or NULL if it has none. */
const char *hero_spell_name(HeroFlavor f, int spell);

/**
 * @return nonzero if @p spell is a real, castable spell in @p f.
 *
 * Diablo's spelldata[] has 37 entries and Hellfire's 52. An id past the end is
 * not merely unknown -- the game indexes that table directly from the player's
 * spell state (Source/control.cpp reads spelldata[j] while drawing the spell
 * book), so a Hellfire id in a Diablo save makes it read past the array.
 */
int hero_spell_exists(HeroFlavor f, int spell);

/**
 * @return nonzero if @p spell can be learned from a book.
 *
 * spelldata[].sBookLvl is -1 for the ones that have no book: the class skills
 * (Repair, Disarm, Recharge, ...) and Identify. The game grants those through
 * _pAblSpells from the class, never through _pMemSpells.
 */
int hero_spell_has_book(HeroFlavor f, int spell);

/**
 * The highest spell level the game itself will produce.
 *
 * Books stop raising a spell at 15 (Source/items.cpp), so anything above that
 * is reachable only by editing.
 */
int hero_spell_max_level(void);

/** Read a spell level, routing ids 37..46 to pSplLvl2. */
int hero_get_spell_level(const PkPlayerStruct *h, int spell);

/**
 * Set a spell level and its spell-book bit, routing ids 37..46 to pSplLvl2.
 * Refuses ids that a save cannot carry or that @p f does not define.
 */
int hero_set_spell_level(PkPlayerStruct *h, HeroFlavor f, int spell, int level,
    char *err);

/* ---- gold ---- */

/** Sum of every gold stack actually held. This is what the game trusts. */
int hero_gold_in_stacks(const PkPlayerStruct *h);

/** Inventory grid cells not occupied by any item. */
int hero_free_inv_cells(const PkPlayerStruct *h);

/** Largest total hero_set_gold could store given current free space. */
int hero_gold_capacity(const PkPlayerStruct *h);

/**
 * Rewrite the character's gold to total exactly @p total.
 *
 * Removes existing gold stacks (compacting InvList and fixing up InvGrid),
 * then lays down ceil(total / GOLD_MAX_LIMIT) new stacks in free cells and
 * sets the cached pGold to match. Non-gold items are left untouched.
 *
 * @return nonzero on success; on failure @p h is unmodified.
 */
int hero_set_gold(PkPlayerStruct *h, int total, char *err);

/* ---- validation ---- */

/** @return nonzero if @p name is a legal character name. */
int hero_name_valid(const char *name, char *err);

/**
 * Check the whole character and append every finding to @p dl.
 *
 * Reports all problems rather than stopping at the first, which is what makes
 * a standalone `validate` useful: fixing one error and re-running to discover
 * the next is a poor way to repair a file. Includes inventory coherence --
 * grid cells pointing at live items, live items reachable from the grid --
 * which nothing else checks.
 */
void hero_check(const PkPlayerStruct *h, HeroFlavor f, DiagList *dl);

/**
 * The spell half of hero_check, exposed so it can be exercised on its own.
 *
 * Called by hero_check; there is no need to call both.
 */
void hero_check_spells(const PkPlayerStruct *h, HeroFlavor f, DiagList *dl);

/**
 * Convenience wrapper over hero_check for callers that only need a verdict.
 * @return nonzero if valid; otherwise err holds the first error.
 */
int hero_validate(const PkPlayerStruct *h, HeroFlavor f, char *err);

/**
 * Report conditions that are legal but will surprise the player -- an
 * experience/level mismatch, a cached pGold that disagrees with the stacks,
 * or a class that does not exist in this flavor. Calls @p warn per finding.
 */
void hero_warnings(const PkPlayerStruct *h, HeroFlavor f, void (*warn)(const char *));

#endif /* BUTCHER_HERO_H */
