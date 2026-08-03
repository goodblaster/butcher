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
		per_level = 0 + 1; /* the table says 0, but single player still adds one */
	else
		per_level = 128 + 1;
	raw += per_level * (hero_max_level() - 1);

	return raw >> HERO_FIXED_SHIFT;
}

/**
 * Source/player.cpp CalcStatDiff -- how many points the class can still take.
 * NextPlrLevel uses it to stop handing out points a character cannot spend.
 */
static int stat_headroom(const PkPlayerStruct *h, HeroFlavor f)
{
	int total = 0;
	const BYTE *base[4] = { &h->pBaseStr, &h->pBaseMag, &h->pBaseDex, &h->pBaseVit };
	for (int i = 0; i < 4; i++) {
		int room = hero_max_stat(f, h->pClass, i) - (int)*base[i];
		if (room > 0)
			total += room;
	}
	return total;
}

int hero_next_exper(const PkPlayerStruct *h)
{
	/* _pNextExper is ExpLvlsTbl[_pLevel], i.e. what the next level costs. */
	return hero_exp_for_level(h->pLevel + 1);
}

int hero_level_up(PkPlayerStruct *h, HeroFlavor f, int level, char *err)
{
	if (level < 1 || level > hero_max_level()) {
		seterr(err, "level must be 1..%d", hero_max_level());
		return 0;
	}
	if (level <= h->pLevel)
		return 0;

	int gained = 0;
	while (h->pLevel < level) {
		/*
		 * Source/player.cpp NextPlrLevel, once per level. Single player, so
		 * each bump is one larger than the multiplayer figure.
		 */
		int diff = stat_headroom(h, f);
		int pts = (int)h->pStatPts;
		if (diff < 5)
			pts = diff; /* a set, not an add -- the game does the same */
		else
			pts += 5;
		if (pts < 0)
			pts = 0;
		if (pts > 255)
			pts = 255; /* the packed field is a byte */
		h->pStatPts = (BYTE)pts;

		int hp = (h->pClass == PC_SORCERER ? 64 : 128) + 1;
		h->pMaxHPBase += hp;
		h->pHPBase = h->pMaxHPBase;

		int mana;
		if (h->pClass == PC_WARRIOR)
			mana = 64;
		else if (h->pClass == 5 /* Barbarian */)
			mana = 0;
		else
			mana = 128;
		mana += 1;
		h->pMaxManaBase += mana;
		h->pManaBase = h->pMaxManaBase;

		h->pLevel++;
		gained++;
	}

	/* The experience the game expects a character of this level to hold. */
	h->pExperience = hero_exp_for_level(level);
	return gained;
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

/** The spelldata[] row for @p spell, or NULL if this flavor has no such id. */
static const SpellData *spell_row(HeroFlavor f, int spell)
{
	if (spell < 0 || spell >= hero_spell_slots(f))
		return NULL;
	return f == FLAVOR_HELLFIRE ? &spelldata_hf[spell] : &spelldata[spell];
}

int hero_spell_exists(HeroFlavor f, int spell)
{
	const SpellData *sd = spell_row(f, spell);
	/* Id 0 is SPL_NULL, a placeholder with no name and nothing to cast. */
	return spell > 0 && sd != NULL && sd->sNameText != NULL;
}

int hero_spell_has_book(HeroFlavor f, int spell)
{
	const SpellData *sd = spell_row(f, spell);
	if (!hero_spell_exists(f, spell))
		return 0;
	return sd->sBookLvl != -1;
}

int hero_spell_max_level(void)
{
	return MAX_SPELL_LEVEL;
}

int hero_get_spell_level(const PkPlayerStruct *h, int spell)
{
	if (spell < 0 || spell >= hero_spell_persisted())
		return 0;
	if (spell < MAX_SPELLS)
		return h->pSplLvl[spell];
	return h->pSplLvl2[spell - MAX_SPELLS];
}

int hero_set_spell_level(PkPlayerStruct *h, HeroFlavor f, int spell, int level,
    char *err)
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
	/*
	 * The id has to exist in the game this save belongs to. Diablo's
	 * spelldata[] has 37 rows, so id 37 is not an unknown spell -- it is a
	 * read past the end of the table every time the game touches the spell
	 * book.
	 *
	 * Clearing is always allowed, whatever the id: refusing that would leave
	 * an already-damaged save with no way to repair it.
	 */
	if (level != 0 && !hero_spell_exists(f, spell)) {
		seterr(err, "spell id %d does not exist in %s, which defines ids 1..%d; "
		            "the game indexes its spell table directly and would read "
		            "past the end of it",
		    spell, hero_flavor_name(f), hero_spell_slots(f) - 1);
		return 0;
	}
	if (level < 0 || level > hero_spell_max_level()) {
		seterr(err, "spell level must be 0..%d", hero_spell_max_level());
		return 0;
	}

	if (spell < MAX_SPELLS)
		h->pSplLvl[spell] = (char)level;
	else
		h->pSplLvl2[spell - MAX_SPELLS] = (char)level;

	/*
	 * Only spells with a book can sit in _pMemSpells. The game masks that
	 * field down to exactly those (Source/player.cpp), so a bit for a class
	 * skill would be dropped anyway -- better to never write it.
	 */
	if (level > 0 && hero_spell_has_book(f, spell))
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
			    "%d, but experience %d is worth level %d -- and the game will "
			    "throw the difference away. ValidatePlayer runs every tick and "
			    "clamps experience down to %d, what level %d needs for its next "
			    "level, so this reverts on the first frame of play. Raise the "
			    "level itself instead (--level %d), which grants the stat "
			    "points and life that come with it",
			    h->pLevel, h->pExperience, implied, hero_next_exper(h), h->pLevel,
			    implied);
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

	hero_check_spells(h, f, dl);
}

/**
 * Everything the game assumes about a character's spells.
 *
 * The game reads spelldata[] straight off these fields, so an id this flavor
 * does not define is an out-of-bounds access rather than a cosmetic mistake.
 * The rules here are the ones Source/player.cpp applies to itself: keep only
 * spells with a book in _pMemSpells, and cap levels at MAX_SPELL_LEVEL.
 */
void hero_check_spells(const PkPlayerStruct *h, HeroFlavor f, DiagList *dl)
{
	int slots = hero_spell_slots(f);
	int stray = 0, first_stray = -1;
	int nobook = 0, first_nobook = -1;
	int overcap = 0, first_overcap = -1;
	int negative = 0, first_negative = -1;
	int orphan_lvl = 0, first_orphan = -1;

	for (int s = 1; s < hero_spell_persisted(); s++) {
		int lvl = hero_get_spell_level(h, s);
		int bit = (h->pMemSpells & SPELLBIT(s)) != 0;

		if (lvl == 0 && !bit)
			continue;

		/* An id this game does not define. */
		if (s >= slots || hero_spell_name(f, s) == NULL) {
			stray++;
			if (first_stray < 0)
				first_stray = s;
			continue;
		}

		if (lvl < 0) {
			negative++;
			if (first_negative < 0)
				first_negative = s;
		} else if (lvl > hero_spell_max_level()) {
			overcap++;
			if (first_overcap < 0)
				first_overcap = s;
		}

		if (bit && !hero_spell_has_book(f, s)) {
			nobook++;
			if (first_nobook < 0)
				first_nobook = s;
		}

		if (lvl > 0 && !bit && hero_spell_has_book(f, s)) {
			orphan_lvl++;
			if (first_orphan < 0)
				first_orphan = s;
		}
	}

	if (first_stray >= 0)
		dl_add(dl, DIAG_ERROR, "spells",
		    "%d spell id%s set that %s does not define (the first is id %d; this "
		    "game has ids 1..%d). The game indexes its spell table directly from "
		    "these fields, so it would read past the end of that table -- clear "
		    "them before loading this character",
		    stray, stray == 1 ? " is" : "s are", hero_flavor_name(f), first_stray,
		    slots - 1);

	if (first_negative >= 0)
		dl_add(dl, DIAG_ERROR, "spells",
		    "%d spell%s a negative level (the first is %s, id %d)", negative,
		    negative == 1 ? " has" : "s have",
		    hero_spell_name(f, first_negative), first_negative);

	if (first_overcap >= 0)
		dl_add(dl, DIAG_WARNING, "spells",
		    "%d spell%s above level %d (the first is %s at %d); the game clamps "
		    "spell levels back to %d, so the extra levels will not survive",
		    overcap, overcap == 1 ? " is" : "s are", hero_spell_max_level(),
		    hero_spell_name(f, first_overcap),
		    hero_get_spell_level(h, first_overcap), hero_spell_max_level());

	if (first_nobook >= 0)
		dl_add(dl, DIAG_WARNING, "spells",
		    "%d spell%s in the spell book that cannot be learned from one (the "
		    "first is %s, id %d). The game grants these through the class "
		    "rather than the book and masks _pMemSpells down to book spells on "
		    "load, so %s will disappear",
		    nobook, nobook == 1 ? " is" : "s are",
		    hero_spell_name(f, first_nobook), first_nobook,
		    nobook == 1 ? "it" : "they");

	if (first_orphan >= 0)
		dl_add(dl, DIAG_WARNING, "spells",
		    "%d spell%s a level but is not in the spell book (the first is %s, "
		    "id %d); the character can cast %s from a staff or scroll but will "
		    "not find %s in the book",
		    orphan_lvl, orphan_lvl == 1 ? " has" : "s have",
		    hero_spell_name(f, first_orphan), first_orphan,
		    orphan_lvl == 1 ? "it" : "them", orphan_lvl == 1 ? "it" : "them");

	/* pSplLvl2 is Hellfire's overflow; a Diablo save has no business using it. */
	if (f != FLAVOR_HELLFIRE) {
		for (int i = 0; i < 10; i++) {
			if (h->pSplLvl2[i] == 0)
				continue;
			dl_add(dl, DIAG_ERROR, "spells",
			    "the Hellfire spell area (pSplLvl2) holds data in a Diablo save; "
			    "byte %d is %d and should be 0",
			    i, h->pSplLvl2[i]);
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

/* ------------------------------------------------------------------ */
/* Repair                                                              */
/* ------------------------------------------------------------------ */

/** Clamp @p v into [lo,hi], logging when it moves. */
static int fix_clamp(int v, int lo, int hi, DiagList *log, const char *where,
    const char *what, int *changed)
{
	if (v >= lo && v <= hi)
		return v;
	int out = v < lo ? lo : hi;
	dl_add(log, DIAG_NOTE, where, "%s was %d, now %d", what, v, out);
	(*changed)++;
	return out;
}

int hero_fix(PkPlayerStruct *h, HeroFlavor f, int settle_warnings, DiagList *log)
{
	int changed = 0;

	/* ---- name ---- */
	char nerr[HERO_ERR_LEN];
	char nbuf[PLR_NAME_LEN + 1];
	memcpy(nbuf, h->pName, PLR_NAME_LEN);
	nbuf[PLR_NAME_LEN] = '\0';
	if (!hero_name_valid(nbuf, nerr)) {
		char clean[PLR_NAME_LEN];
		int w = 0;
		for (int i = 0; i < PLR_NAME_LEN - 1 && nbuf[i] != '\0'; i++) {
			unsigned char c = (unsigned char)nbuf[i];
			if (c >= ' ' && c < 127 && c != '/' && c != '\\' && c != ':')
				clean[w++] = (char)c;
		}
		clean[w] = '\0';
		if (w == 0)
			snprintf(clean, sizeof(clean), "Nameless");
		dl_add(log, DIAG_NOTE, "name", "\"%s\" was not a usable name, now \"%s\"",
		    nbuf, clean);
		memset(h->pName, 0, PLR_NAME_LEN);
		strncpy(h->pName, clean, PLR_NAME_LEN - 1);
		changed++;
	}

	/*
	 * A class outside the range makes the character unloadable, and there is
	 * no way to recover the intended one -- InitPlayer indexes its tables by
	 * class. Warrior is the only safe landing place, and it is said loudly.
	 */
	if (h->pClass < 0 || h->pClass >= hero_num_classes(f)) {
		dl_add(log, DIAG_NOTE, "class",
		    "class %d does not exist in %s; reset to Warrior. Set it to what "
		    "you actually want -- this is a guess, not a recovery",
		    h->pClass, hero_flavor_name(f));
		h->pClass = PC_WARRIOR;
		changed++;
	}

	/* ---- attributes ---- */
	static const char *const stat_where[4] = { "attributes.strength",
		"attributes.magic", "attributes.dexterity", "attributes.vitality" };
	BYTE *stat[4] = { &h->pBaseStr, &h->pBaseMag, &h->pBaseDex, &h->pBaseVit };
	for (int i = 0; i < 4; i++) {
		int cap = hero_max_stat(f, h->pClass, i);
		if (*stat[i] > cap) {
			dl_add(log, DIAG_NOTE, stat_where[i], "%u was above the %s cap of %d, now %d",
			    *stat[i], hero_class_name(f, h->pClass), cap, cap);
			*stat[i] = (BYTE)cap;
			changed++;
		}
	}

	/* ---- level and experience ---- */
	h->pLevel = (char)fix_clamp(h->pLevel, 1, hero_max_level(), log, "level",
	    "level", &changed);
	if (h->pExperience < 0) {
		dl_add(log, DIAG_NOTE, "experience", "was negative (%d), now 0", h->pExperience);
		h->pExperience = 0;
		changed++;
	} else if ((DWORD)h->pExperience > MAXEXP) {
		dl_add(log, DIAG_NOTE, "experience", "%d was above the cap, now %u",
		    h->pExperience, (unsigned)MAXEXP);
		h->pExperience = (int)MAXEXP;
		changed++;
	}

	/* ---- life and mana ---- */
	if (h->pMaxHPBase < HERO_FROM_WHOLE(1)) {
		dl_add(log, DIAG_NOTE, "life.max", "was below 1, now 1");
		h->pMaxHPBase = HERO_FROM_WHOLE(1);
		changed++;
	}
	if (h->pHPBase > h->pMaxHPBase) {
		dl_add(log, DIAG_NOTE, "life.current", "%d was above the maximum, now %d",
		    HERO_TO_WHOLE(h->pHPBase), HERO_TO_WHOLE(h->pMaxHPBase));
		h->pHPBase = h->pMaxHPBase;
		changed++;
	}
	if (h->pHPBase < HERO_FROM_WHOLE(1)) {
		/* The game rewrites this to 64 on load anyway; do it here so the file
		 * says what the character will actually be. */
		dl_add(log, DIAG_NOTE, "life.current", "was below 1, now 1");
		h->pHPBase = HERO_FROM_WHOLE(1);
		changed++;
	}
	if (h->pManaBase > h->pMaxManaBase) {
		dl_add(log, DIAG_NOTE, "mana.current", "%d was above the maximum, now %d",
		    HERO_TO_WHOLE(h->pManaBase), HERO_TO_WHOLE(h->pMaxManaBase));
		h->pManaBase = h->pMaxManaBase;
		changed++;
	}

	/* ---- position ---- */
	if (h->plrlevel >= hero_num_levels(f)) {
		int last = hero_num_levels(f) - 1;
		dl_add(log, DIAG_NOTE, "position.dungeon_level",
		    "%u is beyond %s, now %d", h->plrlevel, hero_flavor_name(f), last);
		h->plrlevel = (BYTE)last;
		changed++;
	}

	/* ---- spells ---- */
	for (int s = 1; s < hero_spell_persisted(); s++) {
		int lvl = hero_get_spell_level(h, s);
		int bit = (h->pMemSpells & SPELLBIT(s)) != 0;
		if (lvl == 0 && !bit)
			continue;

		if (!hero_spell_exists(f, s)) {
			dl_add(log, DIAG_NOTE, "spells",
			    "spell id %d does not exist in %s; cleared", s, hero_flavor_name(f));
			if (s < MAX_SPELLS)
				h->pSplLvl[s] = 0;
			else
				h->pSplLvl2[s - MAX_SPELLS] = 0;
			h->pMemSpells &= ~SPELLBIT(s);
			changed++;
			continue;
		}

		if (lvl < 0) {
			dl_add(log, DIAG_NOTE, "spells", "%s had a negative level, now 0",
			    hero_spell_name(f, s));
			if (s < MAX_SPELLS)
				h->pSplLvl[s] = 0;
			else
				h->pSplLvl2[s - MAX_SPELLS] = 0;
			changed++;
		} else if (settle_warnings && lvl > hero_spell_max_level()) {
			dl_add(log, DIAG_NOTE, "spells", "%s was level %d, now %d (the game's cap)",
			    hero_spell_name(f, s), lvl, hero_spell_max_level());
			if (s < MAX_SPELLS)
				h->pSplLvl[s] = (char)hero_spell_max_level();
			else
				h->pSplLvl2[s - MAX_SPELLS] = (char)hero_spell_max_level();
			changed++;
		}

		if (settle_warnings && bit && !hero_spell_has_book(f, s)) {
			dl_add(log, DIAG_NOTE, "spells",
			    "%s has no book; removed from the spell book, as the game "
			    "would on load",
			    hero_spell_name(f, s));
			h->pMemSpells &= ~SPELLBIT(s);
			changed++;
		}

		/* A level with no book bit: the level is the deliberate part, so put
		 * the spell in the book rather than throwing the level away. */
		if (settle_warnings && !bit && hero_get_spell_level(h, s) > 0
		    && hero_spell_has_book(f, s)) {
			dl_add(log, DIAG_NOTE, "spells",
			    "%s had a level but was not in the spell book; added",
			    hero_spell_name(f, s));
			h->pMemSpells |= SPELLBIT(s);
			changed++;
		}
	}

	/* Hellfire's overflow area has no meaning in a Diablo save. */
	if (f != FLAVOR_HELLFIRE) {
		for (int i = 0; i < 10; i++) {
			if (h->pSplLvl2[i] == 0)
				continue;
			dl_add(log, DIAG_NOTE, "spells",
			    "cleared the Hellfire spell area, which a Diablo save does not use");
			memset(h->pSplLvl2, 0, sizeof(h->pSplLvl2));
			changed++;
			break;
		}
	}

	/*
	 * ---- inventory ----
	 *
	 * The count is recomputed rather than clamped. Clamping a wild value to 40
	 * only trades one error for forty: the game takes InvList[0.._pNumInv-1]
	 * as live, so the count has to match the items that are actually there.
	 * Slots above it may hold stale entries -- RemoveInvItem leaves those
	 * behind -- so the live run is the leading one.
	 */
	{
		int live = 0;
		while (live < NUM_INV_GRID_ELEM && h->InvList[live].idx != 0xFFFF)
			live++;
		if (h->_pNumInv != live) {
			dl_add(log, DIAG_NOTE, "inventory.count",
			    "was %u but %d item%s actually present; now %d", h->_pNumInv, live,
			    live == 1 ? " is" : "s are", live);
			h->_pNumInv = (BYTE)live;
			changed++;
		}
	}
	/*
	 * Grid cells naming an item that is not live. Clearing the cell is the
	 * conservative repair: the alternative is inventing an item, and a packed
	 * item is a seed the game replays rather than anything we could author.
	 */
	for (int i = 0; i < NUM_INV_GRID_ELEM; i++) {
		int cell = h->InvGrid[i];
		int idx = cell < 0 ? -cell : cell;
		if (idx == 0 || idx <= h->_pNumInv)
			continue;
		dl_add(log, DIAG_NOTE, "inventory.grid",
		    "cell %d pointed at item %d, but only %u are live; cleared", i, idx,
		    h->_pNumInv);
		h->InvGrid[i] = 0;
		changed++;
	}

	/*
	 * Experience the game will not honour. ValidatePlayer clamps it down to
	 * _pNextExper on the first tick, so a save carrying more is describing a
	 * character that will not exist. Doing it here makes the file say what the
	 * game will actually produce -- raising the level instead is a gameplay
	 * decision, not a repair, and hero_check names the command for it.
	 */
	if (settle_warnings) {
		int cap = hero_next_exper(h);
		if (h->pExperience > cap) {
			dl_add(log, DIAG_NOTE, "experience",
			    "%d is more than level %d can hold; the game clamps it to %d on "
			    "load, so that is what it now says. Use --level to raise the "
			    "level for real",
			    h->pExperience, h->pLevel, cap);
			h->pExperience = cap;
			changed++;
		}
	}

	/* ---- gold ---- */
	if (settle_warnings) {
		int stacked = hero_gold_in_stacks(h);
		if (stacked != h->pGold) {
			dl_add(log, DIAG_NOTE, "gold",
			    "cached total was %d but the stacks hold %d; now %d, which is "
			    "what the game would compute",
			    h->pGold, stacked, stacked);
			h->pGold = stacked;
			changed++;
		}
	}

	return changed;
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
