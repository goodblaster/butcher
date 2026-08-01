/**
 * @file hero.cpp
 *
 * The editable view of a character: validation, gold-stack distribution,
 * spell lookup, and the game's own limit tables.
 */
#include "hero.h"

#include "../third_party/devilution/Source/spelldat.h"
#include "../third_party/devilution/Source/itemdat.h"

#include <stdarg.h>

/*
 * Hellfire's copies of the two data tables. Source/spelldat.cpp and
 * Source/itemdat.cpp are compiled a second time with -DHELLFIRE and the
 * array name redefined on the command line, so both flavors' tables live in
 * one binary with no edit to either file. SpellData and ItemDataStruct have
 * no conditional members, so the type is the same on both sides.
 */
extern SpellData spelldata_hf[];

/* Constants lifted from the real headers by compat/hellfire.cpp. */
extern const int hf_num_classes;
extern const int hf_max_spells;
extern const int hf_num_levels;

/*
 * These two tables are copied from Source/player.cpp. They cannot be linked:
 * player.cpp is one translation unit with the entire player implementation in
 * it, so pulling in the data would drag in the whole game. The dimensions are
 * asserted against the real headers below, so a change to NUM_CLASSES or
 * MAXCHARLEVEL breaks this build rather than silently going stale.
 *
 * If a value here ever disagrees with player.cpp, player.cpp is right.
 */

/**
 * Source/player.cpp:166 -- maximum base stats, indexed [class][attribute].
 * The first three rows are Diablo's; Hellfire adds the last three behind its
 * #ifdef. Both are listed here because MaxStats cannot be linked (see above).
 */
static const int kMaxStats[6][4] = {
	{ 250, 50, 60, 100 },  /* warrior   */
	{ 55, 70, 250, 80 },   /* rogue     */
	{ 45, 250, 85, 80 },   /* sorcerer  */
	{ 150, 80, 150, 80 },  /* monk      -- Hellfire */
	{ 120, 120, 120, 100 },/* bard      -- Hellfire */
	{ 255, 0, 55, 150 },   /* barbarian -- Hellfire */
};

/** Source/player.cpp:179 -- experience needed to reach each level. */
static const int kExpLvlsTbl[MAXCHARLEVEL] = {
	0, 2000, 4620, 8040, 12489, 18258,
	25712, 35309, 47622, 63364, 83419, 108879,
	141086, 181683, 231075, 313656, 424067, 571190,
	766569, 1025154, 1366227, 1814568, 2401895, 3168651,
	4166200, 5459523, 7130496, 9281874, 12042092, 15571031,
	20066900, 25774405, 32994399, 42095202, 53525811, 67831218,
	85670061, 107834823, 135274799, 169122009, 210720231, 261657253,
	323800420, 399335440, 490808349, 601170414, 733825617, 892680222,
	1082908612, 1310707109, 1583495809,
};

CT_ASSERT(NUM_CLASSES == 3, hero_three_diablo_classes);
CT_ASSERT(MAXCHARLEVEL == 51, hero_level_cap_51);
CT_ASSERT(ATTRIB_STR == 0 && ATTRIB_MAG == 1 && ATTRIB_DEX == 2 && ATTRIB_VIT == 3,
    hero_attrib_order);
CT_ASSERT(IDI_GOLD == 0, hero_gold_is_index_zero);

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

static void seterr(char *err, const char *fmt, ...)
{
	if (err == NULL)
		return;
	va_list va;
	va_start(va, fmt);
	vsnprintf(err, HERO_ERR_LEN, fmt, va);
	va_end(va);
}

HeroFlavor hero_flavor_for_path(const char *path)
{
	size_t n = strlen(path);

	/* Hellfire single-player saves are single_%d.hsv (Source/pfile.cpp:118). */
	if (n >= 4 && strcasecmp(path + n - 4, ".hsv") == 0)
		return FLAVOR_HELLFIRE;
	return FLAVOR_DIABLO;
}

const char *hero_flavor_name(HeroFlavor f)
{
	return f == FLAVOR_HELLFIRE ? "Hellfire" : "Diablo";
}

int hero_max_level(void)
{
	/* MAXCHARLEVEL is 51 in both flavors. */
	return MAXCHARLEVEL - 1;
}

int hero_num_classes(HeroFlavor f)
{
	return f == FLAVOR_HELLFIRE ? hf_num_classes : NUM_CLASSES;
}

int hero_num_levels(HeroFlavor f)
{
	return f == FLAVOR_HELLFIRE ? hf_num_levels : NUMLEVELS;
}

int hero_max_stat(HeroFlavor f, int pclass, int attrib)
{
	if (pclass < 0 || pclass >= hero_num_classes(f) || attrib < 0 || attrib > 3)
		return 0;
	return kMaxStats[pclass][attrib];
}

int hero_exp_for_level(int level)
{
	/* AddPlrExperience counts up from 0 while exp >= ExpLvlsTbl[n], so being
	 * at level L requires at least ExpLvlsTbl[L-1]. */
	if (level <= 1)
		return 0;
	if (level > hero_max_level())
		level = hero_max_level();
	return kExpLvlsTbl[level - 1];
}

int hero_level_for_exp(int exp)
{
	int lvl = 0;
	while (lvl < MAXCHARLEVEL && exp >= kExpLvlsTbl[lvl])
		lvl++;
	if (lvl > hero_max_level())
		lvl = hero_max_level();
	return lvl < 1 ? 1 : lvl;
}

const char *hero_class_name(HeroFlavor f, int pclass)
{
	static const char *names[6] = {
		"Warrior", "Rogue", "Sorcerer", "Monk", "Bard", "Barbarian"
	};

	if (pclass < 0 || pclass >= hero_num_classes(f))
		return "unknown";
	return names[pclass];
}

/*
 * Life and mana ceilings, worked out from the game's own arithmetic.
 *
 * CreatePlayer (Source/player.cpp:802):
 *     life = (vitality + 10) << 6, doubled for a Warrior or Barbarian,
 *                                  x1.5 for a Rogue, Monk or Bard
 *     mana = magic << 6,           doubled for a Sorcerer,
 *                                  x1.75 for a Bard, x1.5 for a Rogue or Monk
 *
 * NextPlrLevel (Source/player.cpp:983) then adds, per level, in single player:
 *     life  129, or 65 for a Sorcerer
 *     mana   65 for a Warrior, 0 for a Barbarian, 129 for everyone else
 *
 * Both are in the fixed-point form; these helpers return whole points.
 */
int hero_max_life(HeroFlavor f, int pclass)
{
	if (pclass < 0 || pclass >= hero_num_classes(f))
		return 0;

	int raw = (hero_max_stat(f, pclass, ATTRIB_VIT) + 10) << HERO_FIXED_SHIFT;
	if (pclass == PC_WARRIOR || pclass == 5 /* Barbarian */)
		raw <<= 1;
	else if (pclass == PC_ROGUE || pclass == 3 /* Monk */ || pclass == 4 /* Bard */)
		raw += raw >> 1;

	int per_level = (pclass == PC_SORCERER ? 64 : 128) + 1;
	raw += per_level * (hero_max_level() - 1);

	return raw >> HERO_FIXED_SHIFT;
}

int hero_max_mana(HeroFlavor f, int pclass)
{
	if (pclass < 0 || pclass >= hero_num_classes(f))
		return 0;

	int raw = hero_max_stat(f, pclass, ATTRIB_MAG) << HERO_FIXED_SHIFT;
	if (pclass == PC_SORCERER)
		raw <<= 1;
	else if (pclass == 4 /* Bard */)
		raw += raw * 3 / 4;
	else if (pclass == PC_ROGUE || pclass == 3 /* Monk */)
		raw += raw >> 1;

	int per_level;
	if (pclass == PC_WARRIOR)
		per_level = 64 + 1;
	else if (pclass == 5 /* Barbarian */)
		per_level = 0; /* a Barbarian gains no mana with levels */
	else
		per_level = 128 + 1;
	raw += per_level * (hero_max_level() - 1);

	return raw >> HERO_FIXED_SHIFT;
}

/* ------------------------------------------------------------------ */
/* Spells                                                              */
/* ------------------------------------------------------------------ */

int hero_spell_slots(HeroFlavor f)
{
	return f == FLAVOR_HELLFIRE ? hf_max_spells : MAX_SPELLS;
}

int hero_spell_persisted(void)
{
	/* pSplLvl[0..36] plus pSplLvl2[0..9] == ids 0..46. */
	return MAX_SPELLS + 10;
}

const char *hero_spell_name(HeroFlavor f, int spell)
{
	if (spell < 0 || spell >= hero_spell_slots(f))
		return NULL;
	return f == FLAVOR_HELLFIRE ? spelldata_hf[spell].sNameText
	                            : spelldata[spell].sNameText;
}

int hero_get_spell_level(const PkPlayerStruct *h, int spell)
{
	if (spell < 0 || spell >= hero_spell_persisted())
		return 0;
	if (spell < MAX_SPELLS)
		return h->pSplLvl[spell];
	return h->pSplLvl2[spell - MAX_SPELLS];
}

int hero_set_spell_level(PkPlayerStruct *h, int spell, int level, char *err)
{
	if (spell <= 0) {
		seterr(err, "spell id %d is not a real spell", spell);
		return 0;
	}
	if (spell >= hero_spell_persisted()) {
		seterr(err, "spell id %d cannot be stored in a save file: PackPlayer "
		            "only writes ids 0..%d (pSplLvl plus pSplLvl2), so the "
		            "game would lose it",
		    spell, hero_spell_persisted() - 1);
		return 0;
	}
	if (level < 0 || level > 127) {
		seterr(err, "spell level must be 0..127");
		return 0;
	}

	if (spell < MAX_SPELLS)
		h->pSplLvl[spell] = (char)level;
	else
		h->pSplLvl2[spell - MAX_SPELLS] = (char)level;

	if (level > 0)
		h->pMemSpells |= SPELLBIT(spell);
	else
		h->pMemSpells &= ~SPELLBIT(spell);
	return 1;
}

/** Compare ignoring case, spaces, hyphens and underscores. */
static int loose_equal(const char *a, const char *b)
{
	for (;;) {
		while (*a == ' ' || *a == '-' || *a == '_')
			a++;
		while (*b == ' ' || *b == '-' || *b == '_')
			b++;
		if (*a == '\0' || *b == '\0')
			return *a == *b;
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
		a++;
		b++;
	}
}

int hero_find_spell(HeroFlavor f, const char *name)
{
	int slots = hero_spell_slots(f);
	char *end;
	long n;

	/* A bare number is taken as a spell id. */
	n = strtol(name, &end, 10);
	if (*name != '\0' && *end == '\0') {
		if (n > 0 && n < slots)
			return (int)n;
		return -1;
	}

	for (int i = 1; i < slots; i++) {
		const char *t = hero_spell_name(f, i);
		if (t != NULL && loose_equal(t, name))
			return i;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* Inventory grid                                                      */
/* ------------------------------------------------------------------ */

/*
 * InvGrid holds 1-based indices into InvList: positive marks an item's
 * origin cell, negative a continuation cell of a multi-cell item, zero a
 * free cell (Source/inv.cpp:1379). _pNumInv counts the live InvList entries.
 */

int hero_free_inv_cells(const PkPlayerStruct *h)
{
	int n = 0;
	for (int i = 0; i < NUM_INV_GRID_ELEM; i++)
		if (h->InvGrid[i] == 0)
			n++;
	return n;
}

int hero_gold_in_stacks(const PkPlayerStruct *h)
{
	int total = 0;
	int n = h->_pNumInv;

	if (n > NUM_INV_GRID_ELEM)
		n = NUM_INV_GRID_ELEM;

	/* CalculateGold (Source/inv.cpp:2965) sums the belt and the first
	 * _pNumInv inventory entries. */
	for (int i = 0; i < MAXBELTITEMS; i++)
		if (h->SpdList[i].idx == IDI_GOLD)
			total += h->SpdList[i].wValue;
	for (int i = 0; i < n; i++)
		if (h->InvList[i].idx == IDI_GOLD)
			total += h->InvList[i].wValue;
	return total;
}

int hero_gold_capacity(const PkPlayerStruct *h)
{
	/* Cells currently holding gold are reusable, plus whatever is free. */
	int cells = hero_free_inv_cells(h);
	int n = h->_pNumInv;

	if (n > NUM_INV_GRID_ELEM)
		n = NUM_INV_GRID_ELEM;
	for (int i = 0; i < n; i++)
		if (h->InvList[i].idx == IDI_GOLD)
			cells++;
	return cells * GOLD_MAX_LIMIT;
}

int hero_set_gold(PkPlayerStruct *h, int total, char *err)
{
	PkPlayerStruct work = *h;
	int remap[NUM_INV_GRID_ELEM];
	int kept = 0;
	int n = h->_pNumInv;

	if (total < 0) {
		seterr(err, "gold cannot be negative");
		return 0;
	}
	if (n > NUM_INV_GRID_ELEM) {
		seterr(err, "_pNumInv is %d, beyond the %d inventory slots -- refusing to "
		            "edit a malformed inventory",
		    n, NUM_INV_GRID_ELEM);
		return 0;
	}

	/* Drop existing gold, compacting InvList. */
	for (int i = 0; i < NUM_INV_GRID_ELEM; i++)
		remap[i] = -1;
	for (int i = 0; i < n; i++) {
		if (h->InvList[i].idx == IDI_GOLD)
			continue;
		work.InvList[kept] = h->InvList[i];
		remap[i] = kept;
		kept++;
	}
	for (int i = kept; i < NUM_INV_GRID_ELEM; i++) {
		memset(&work.InvList[i], 0, sizeof(work.InvList[i]));
		work.InvList[i].idx = 0xFFFF;
	}

	/* Rewrite the grid against the new indices, freeing gold cells. */
	for (int c = 0; c < NUM_INV_GRID_ELEM; c++) {
		int v = h->InvGrid[c];
		if (v == 0) {
			work.InvGrid[c] = 0;
			continue;
		}
		int old = (v > 0 ? v : -v) - 1;
		if (old < 0 || old >= n || remap[old] < 0) {
			work.InvGrid[c] = 0; /* was gold, or dangling */
			continue;
		}
		work.InvGrid[c] = (char)(v > 0 ? remap[old] + 1 : -(remap[old] + 1));
	}
	work._pNumInv = (BYTE)kept;

	/* Lay down new stacks. */
	int stacks = (total + GOLD_MAX_LIMIT - 1) / GOLD_MAX_LIMIT;
	int free_cells = 0;
	for (int c = 0; c < NUM_INV_GRID_ELEM; c++)
		if (work.InvGrid[c] == 0)
			free_cells++;

	if (stacks > free_cells) {
		seterr(err, "%d gold needs %d stacks but only %d inventory cells are free "
		            "(max %d here; a stack holds %d)",
		    total, stacks, free_cells, free_cells * GOLD_MAX_LIMIT, GOLD_MAX_LIMIT);
		return 0;
	}
	if (kept + stacks > NUM_INV_GRID_ELEM) {
		seterr(err, "%d stacks would exceed the %d inventory entries",
		    stacks, NUM_INV_GRID_ELEM);
		return 0;
	}

	/*
	 * The game's gold auto-place walks cells from 39 down to 0
	 * (Source/inv.cpp:922); match it so the result looks like something the
	 * game produced. Seeds are deterministic but distinct -- gold seeds are
	 * only ever used to tell stacks apart, and VerifyGoldSeeds
	 * (Source/pack.cpp) re-rolls duplicates on load anyway.
	 */
	uint32_t seed = 0x1D3A5B79u;
	int left = total;
	int placed = 0;

	for (int c = NUM_INV_GRID_ELEM - 1; c >= 0 && left > 0; c--) {
		if (work.InvGrid[c] != 0)
			continue;

		int amount = left > GOLD_MAX_LIMIT ? GOLD_MAX_LIMIT : left;
		int slot = kept + placed;

		seed = seed * 1103515245u + 12345u;

		memset(&work.InvList[slot], 0, sizeof(work.InvList[slot]));
		work.InvList[slot].idx = IDI_GOLD;
		work.InvList[slot].iSeed = (DWORD)seed;
		work.InvList[slot].iCreateInfo = 0;
		work.InvList[slot].wValue = (WORD)amount;

		work.InvGrid[c] = (char)(slot + 1);
		placed++;
		left -= amount;
	}

	work._pNumInv = (BYTE)(kept + placed);
	work.pGold = total;

	*h = work;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

int hero_name_valid(const char *name, char *err)
{
	size_t n = strlen(name);

	if (n == 0) {
		seterr(err, "name cannot be empty");
		return 0;
	}
	if (n >= PLR_NAME_LEN) {
		seterr(err, "name is %zu characters; the limit is %d", n, PLR_NAME_LEN - 1);
		return 0;
	}
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)name[i];
		if (c < 0x20 || c == 0x7F) {
			seterr(err, "name contains a control character");
			return 0;
		}
		/* pfile derives filenames from save slots, not names, but a name with
		 * a separator in it still has no business being here. */
		if (c == '/' || c == '\\' || c == ':') {
			seterr(err, "name cannot contain '%c'", c);
			return 0;
		}
	}
	return 1;
}

void hero_check(const PkPlayerStruct *h, HeroFlavor f, DiagList *dl)
{
	char name[PLR_NAME_LEN + 1];
	char nerr[HERO_ERR_LEN];

	memcpy(name, h->pName, PLR_NAME_LEN);
	name[PLR_NAME_LEN] = '\0';
	if (!hero_name_valid(name, nerr))
		dl_add(dl, DIAG_ERROR, "name", "%s", nerr);

	if (h->pClass < 0 || h->pClass >= hero_num_classes(f)) {
		if (f == FLAVOR_DIABLO && h->pClass >= 0 && h->pClass < 6)
			/* Do not name a file extension here: this same check runs on
			 * saves, JSON documents and raw structs, and only --hellfire
			 * applies to all three. */
			dl_add(dl, DIAG_ERROR, "class",
			    "class %d (%s) only exists in Hellfire, but this is being read "
			    "as Diablo; pass --hellfire",
			    h->pClass, hero_class_name(FLAVOR_HELLFIRE, h->pClass));
		else
			dl_add(dl, DIAG_ERROR, "class", "class %d is not one of 0..%d",
			    h->pClass, hero_num_classes(f) - 1);
	} else {
		/* Stat caps only mean something once the class is known. */
		static const struct {
			int attrib;
			const char *label;
		} attribs[4] = {
			{ ATTRIB_STR, "strength" },
			{ ATTRIB_MAG, "magic" },
			{ ATTRIB_DEX, "dexterity" },
			{ ATTRIB_VIT, "vitality" },
		};
		const BYTE vals[4] = { h->pBaseStr, h->pBaseMag, h->pBaseDex, h->pBaseVit };
		for (int i = 0; i < 4; i++) {
			int cap = hero_max_stat(f, h->pClass, attribs[i].attrib);
			if (vals[i] > cap) {
				char where[64];
				snprintf(where, sizeof(where), "attributes.%s", attribs[i].label);
				dl_add(dl, DIAG_ERROR, where, "%u exceeds the %s cap of %d",
				    vals[i], hero_class_name(f, h->pClass), cap);
			}
		}
	}

	if (h->pLevel < 1 || h->pLevel > hero_max_level())
		dl_add(dl, DIAG_ERROR, "level", "%d is outside 1..%d", h->pLevel,
		    hero_max_level());

	if (h->pExperience < 0)
		dl_add(dl, DIAG_ERROR, "experience", "cannot be negative");
	else if ((DWORD)h->pExperience > MAXEXP)
		dl_add(dl, DIAG_ERROR, "experience", "%d exceeds the game's cap of %u",
		    h->pExperience, (unsigned)MAXEXP);

	if (h->pMaxHPBase < HERO_FROM_WHOLE(1))
		dl_add(dl, DIAG_ERROR, "life.max", "must be at least 1");
	if (h->pHPBase > h->pMaxHPBase)
		dl_add(dl, DIAG_ERROR, "life.current", "%d exceeds the maximum of %d",
		    HERO_TO_WHOLE(h->pHPBase), HERO_TO_WHOLE(h->pMaxHPBase));
	/*
	 * UnPackPlayer clamps life below 64 up to 64 unless killok, so a lower
	 * value silently changes the moment the game loads it.
	 */
	if (h->pHPBase < HERO_FROM_WHOLE(1))
		dl_add(dl, DIAG_ERROR, "life.current",
		    "below 1; the game rewrites this on load");
	if (h->pManaBase > h->pMaxManaBase)
		dl_add(dl, DIAG_ERROR, "mana.current", "%d exceeds the maximum of %d",
		    HERO_TO_WHOLE(h->pManaBase), HERO_TO_WHOLE(h->pMaxManaBase));

	if (h->_pNumInv > NUM_INV_GRID_ELEM)
		dl_add(dl, DIAG_ERROR, "inventory.count", "%u is beyond the %d slots",
		    h->_pNumInv, NUM_INV_GRID_ELEM);

	for (int i = 0; i < hero_spell_persisted(); i++) {
		if (hero_get_spell_level(h, i) < 0) {
			char where[64];
			snprintf(where, sizeof(where), "spells (id %d)", i);
			dl_add(dl, DIAG_ERROR, where, "level is negative");
		}
	}

	if (h->plrlevel >= hero_num_levels(f))
		dl_add(dl, DIAG_ERROR, "position.dungeon_level",
		    "%u is beyond 0..%d for %s", h->plrlevel, hero_num_levels(f) - 1,
		    hero_flavor_name(f));

	/*
	 * Inventory coherence. InvGrid holds 1-based InvList indices, positive at
	 * an item's origin cell and negative on its continuation cells. An
	 * incoherent grid is how a hand-edited inventory goes wrong, and the game
	 * will read whatever the indices point at.
	 */
	if (h->_pNumInv <= NUM_INV_GRID_ELEM) {
		int origin_seen[NUM_INV_GRID_ELEM];
		memset(origin_seen, 0, sizeof(origin_seen));

		for (int c = 0; c < NUM_INV_GRID_ELEM; c++) {
			int v = h->InvGrid[c];
			if (v == 0)
				continue;
			int idx = (v > 0 ? v : -v) - 1;
			char where[64];
			snprintf(where, sizeof(where), "inventory.grid cell %d", c);
			if (idx < 0 || idx >= h->_pNumInv) {
				dl_add(dl, DIAG_ERROR, where,
				    "points at item %d, but only %u items are live",
				    idx + 1, h->_pNumInv);
				continue;
			}
			if (v > 0)
				origin_seen[idx] = 1;
		}
		for (int i = 0; i < h->_pNumInv; i++) {
			char where[64];
			snprintf(where, sizeof(where), "inventory.items slot %d", i);
			if (h->InvList[i].idx == 0xFFFF)
				dl_add(dl, DIAG_ERROR, where,
				    "is empty but sits below the item count of %u", h->_pNumInv);
			else if (!origin_seen[i])
				dl_add(dl, DIAG_ERROR, where,
				    "has no cell in the grid, so the game cannot show it");
		}
		/*
		 * Slots above the count are NOT reported. Real saves routinely carry
		 * stale items there: RemoveInvItem decrements _pNumInv and shifts the
		 * list down without clearing the vacated tail, and PackPlayer writes
		 * all 40 slots regardless. The game ignores them, so flagging them
		 * would put eight warnings on a perfectly ordinary character -- which
		 * is how warnings get ignored. A grid cell pointing past the count is
		 * a different matter and is an error above.
		 */
	}

	/* Advisories: legal, but probably not intended. */
	int stacked = hero_gold_in_stacks(h);
	if (stacked != h->pGold)
		dl_add(dl, DIAG_WARNING, "gold",
		    "cached total is %d but the inventory holds %d; the game recomputes "
		    "this from the stacks, so %d is what you will actually have",
		    h->pGold, stacked, stacked);

	int implied = hero_level_for_exp(h->pExperience);
	if (h->pLevel >= 1 && h->pLevel <= hero_max_level()) {
		if (implied < h->pLevel)
			dl_add(dl, DIAG_WARNING, "level",
			    "%d, but experience %d is only worth level %d. The character "
			    "keeps level %d and cannot gain another until experience passes "
			    "%d; set experience to %d to make it consistent",
			    h->pLevel, h->pExperience, implied, h->pLevel,
			    hero_exp_for_level(h->pLevel + 1), hero_exp_for_level(h->pLevel));
		else if (implied > h->pLevel)
			dl_add(dl, DIAG_WARNING, "level",
			    "%d, but experience %d is worth level %d; the game will level "
			    "the character up on its next experience gain",
			    h->pLevel, h->pExperience, implied);
	}

	if (f == FLAVOR_HELLFIRE) {
		for (int s = hero_spell_persisted(); s < hero_spell_slots(f); s++) {
			if ((h->pMemSpells & SPELLBIT(s)) == 0)
				continue;
			const char *n = hero_spell_name(f, s);
			dl_add(dl, DIAG_WARNING, "spells",
			    "the book has %s (id %d) set, but only ids 0..%d are written to "
			    "a save, so the game will drop it",
			    n != NULL ? n : "a spell", s, hero_spell_persisted() - 1);
			break;
		}
	}
}

int hero_validate(const PkPlayerStruct *h, HeroFlavor f, char *err)
{
	DiagList dl;
	dl_init(&dl);
	hero_check(h, f, &dl);

	int ok = 1;
	for (int i = 0; i < dl.n; i++) {
		if (dl.items[i].level != DIAG_ERROR)
			continue;
		if (dl.items[i].where[0] != '\0')
			snprintf(err, HERO_ERR_LEN, "%s: %s", dl.items[i].where,
			    dl.items[i].msg);
		else
			snprintf(err, HERO_ERR_LEN, "%s", dl.items[i].msg);
		ok = 0;
		break;
	}
	dl_free(&dl);
	return ok;
}

void hero_warnings(const PkPlayerStruct *h, HeroFlavor f, void (*warn)(const char *))
{
	DiagList dl;
	dl_init(&dl);
	hero_check(h, f, &dl);

	for (int i = 0; i < dl.n; i++) {
		if (dl.items[i].level != DIAG_WARNING)
			continue;
		char buf[DIAG_MSG_LEN + DIAG_WHERE_LEN + 4];
		if (dl.items[i].where[0] != '\0')
			snprintf(buf, sizeof(buf), "%s: %s", dl.items[i].where, dl.items[i].msg);
		else
			snprintf(buf, sizeof(buf), "%s", dl.items[i].msg);
		warn(buf);
	}
	dl_free(&dl);
}
