/**
 * @file edit_check.cpp
 *
 * Phase 4 gate: the editor itself -- validation against the game's limits,
 * gold-stack distribution, spell lookup, and the experience/level rules.
 *
 * The riskiest thing here is gold. It is not a field, it is a derived total
 * (CalculateGold, Source/inv.cpp:2965), so setting it means rewriting the
 * inventory: removing old stacks, compacting InvList, remapping InvGrid, and
 * laying down new stacks. A mistake corrupts the inventory rather than merely
 * writing a wrong number, so the invariants are checked exhaustively.
 */
#include "../src/format.h"

#include "../third_party/devilution/Source/encrypt.h"

#include <stdarg.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* Harness                                                             */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail;

static void ok(int cond, const char *what)
{
	if (cond) {
		g_pass++;
		printf("  ok    %s\n", what);
	} else {
		g_fail++;
		printf("  FAIL  %s\n", what);
	}
}

static void okf(int cond, const char *fmt, ...)
{
	char buf[256];
	va_list va;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	ok(cond, buf);
}

static void section(const char *name)
{
	printf("\n%s\n", name);
}

static const char *g_tmpdir;

static const char *tmppath(const char *leaf)
{
	static char buf[1024];
	snprintf(buf, sizeof(buf), "%s/%s", g_tmpdir, leaf);
	return buf;
}

/** Counts hero_warnings callbacks. */
static int g_warns;
static void count_warn(const char *msg)
{
	(void)msg;
	g_warns++;
}

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

static void base_hero(PkPlayerStruct *p, int pclass)
{
	memset(p, 0, sizeof(*p));
	strcpy(p->pName, "Testy");
	p->pClass = (char)pclass;
	p->pLevel = 10;
	p->pExperience = hero_exp_for_level(10);
	p->pBaseStr = 30;
	p->pBaseMag = 30;
	p->pBaseDex = 30;
	p->pBaseVit = 30;
	p->pHPBase = HERO_FROM_WHOLE(50);
	p->pMaxHPBase = HERO_FROM_WHOLE(50);
	p->pManaBase = HERO_FROM_WHOLE(30);
	p->pMaxManaBase = HERO_FROM_WHOLE(30);
	p->plrlevel = 3;
	for (int i = 0; i < NUM_INVLOC; i++)
		p->InvBody[i].idx = 0xFFFF;
	for (int i = 0; i < NUM_INV_GRID_ELEM; i++)
		p->InvList[i].idx = 0xFFFF;
	for (int i = 0; i < MAXBELTITEMS; i++)
		p->SpdList[i].idx = 0xFFFF;
}

/** Put a non-gold 1x1 item in InvList slot `slot`, occupying grid cell `cell`. */
static void add_item(PkPlayerStruct *p, int slot, int cell, int idx)
{
	memset(&p->InvList[slot], 0, sizeof(p->InvList[slot]));
	p->InvList[slot].idx = (WORD)idx;
	p->InvList[slot].bDur = 30;
	p->InvList[slot].bMDur = 30;
	p->InvGrid[cell] = (char)(slot + 1);
	if (slot + 1 > p->_pNumInv)
		p->_pNumInv = (BYTE)(slot + 1);
}

/**
 * Verify the inventory is internally consistent: grid entries point at live
 * InvList slots, live slots are reachable from the grid, and slots at or
 * beyond _pNumInv are empty.
 */
static int inv_consistent(const PkPlayerStruct *h, char *why, size_t n)
{
	int seen[NUM_INV_GRID_ELEM];
	memset(seen, 0, sizeof(seen));

	if (h->_pNumInv > NUM_INV_GRID_ELEM) {
		snprintf(why, n, "_pNumInv %u out of range", h->_pNumInv);
		return 0;
	}
	for (int c = 0; c < NUM_INV_GRID_ELEM; c++) {
		int v = h->InvGrid[c];
		if (v == 0)
			continue;
		int idx = (v > 0 ? v : -v) - 1;
		if (idx < 0 || idx >= h->_pNumInv) {
			snprintf(why, n, "grid cell %d references slot %d, outside 0..%u",
			    c, idx, h->_pNumInv);
			return 0;
		}
		if (v > 0)
			seen[idx] = 1;
	}
	for (int i = 0; i < h->_pNumInv; i++) {
		if (h->InvList[i].idx == 0xFFFF) {
			snprintf(why, n, "slot %d is empty but below _pNumInv %u", i, h->_pNumInv);
			return 0;
		}
		if (!seen[i]) {
			snprintf(why, n, "slot %d has no origin cell in the grid", i);
			return 0;
		}
	}
	for (int i = h->_pNumInv; i < NUM_INV_GRID_ELEM; i++) {
		if (h->InvList[i].idx != 0xFFFF) {
			snprintf(why, n, "slot %d is above _pNumInv %u but not empty",
			    i, h->_pNumInv);
			return 0;
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* 0. Creating a character                                             */
/* ------------------------------------------------------------------ */

/*
 * hero_create reproduces CreatePlayer. The figures below are the game's own
 * (Source/player.cpp): starting stats from the four tables, life
 * (vitality + 10) << 6 with a class multiplier, mana magic << 6 likewise.
 */
static void check_create(void)
{
	section("0. creating a character");

	char err[HERO_ERR_LEN];
	PkPlayerStruct h;

	ok(hero_create(&h, FLAVOR_DIABLO, PC_WARRIOR, "Aidan", err), "a Warrior");
	ok(h.pBaseStr == 30 && h.pBaseMag == 10 && h.pBaseDex == 20 && h.pBaseVit == 25,
	    "  ...with the game's starting stats");
	ok(HERO_TO_WHOLE(h.pMaxHPBase) == 70, "  ...70 life: (25+10) doubled");
	ok(HERO_TO_WHOLE(h.pMaxManaBase) == 10, "  ...and 10 mana");
	ok(h.pLevel == 1 && h.pExperience == 0 && h.pStatPts == 0, "  ...at level 1");
	ok(h.pHPBase == h.pMaxHPBase && h.pManaBase == h.pMaxManaBase, "  ...at full");
	ok(hero_validate(&h, FLAVOR_DIABLO, err), "  ...and it validates");

	/* Empty slots are 0xFFFF; a zeroed one reads as item index 0, which is gold. */
	int empty = 1;
	for (int i = 0; i < NUM_INV_GRID_ELEM; i++)
		if (h.InvList[i].idx != 0xFFFF)
			empty = 0;
	for (int i = 0; i < NUM_INVLOC; i++)
		if (h.InvBody[i].idx != 0xFFFF)
			empty = 0;
	ok(empty && h._pNumInv == 0 && h.pGold == 0,
	    "  ...carrying nothing, with every slot marked empty rather than zeroed");

	/* Only a Sorcerer starts knowing a spell. */
	ok(hero_create(&h, FLAVOR_DIABLO, PC_SORCERER, "Jazreth", err), "a Sorcerer");
	ok(h.pSplLvl[SPL_FIREBOLT] == 2 && (h.pMemSpells & SPELLBIT(SPL_FIREBOLT)) != 0,
	    "  ...starts with Firebolt at level 2");
	ok(HERO_TO_WHOLE(h.pMaxManaBase) == 70, "  ...and 70 mana: 35 doubled");

	ok(hero_create(&h, FLAVOR_DIABLO, PC_ROGUE, "Moreina", err), "a Rogue");
	ok(h.pMemSpells == 0, "  ...knows no spells");
	ok(HERO_TO_WHOLE(h.pMaxHPBase) == 45, "  ...45 life: (20+10) and half again");

	/* Hellfire classes exist only there. */
	ok(!hero_create(&h, FLAVOR_DIABLO, 3, "Jazreth", err),
	    "a Monk is refused in Diablo");
	ok(hero_create(&h, FLAVOR_HELLFIRE, 3, "Jazreth", err), "  ...and allowed in Hellfire");
	ok(hero_create(&h, FLAVOR_HELLFIRE, 5, "Grognak", err), "a Barbarian");
	ok(h.pBaseMag == 0, "  ...starts with no magic at all");

	/* Bad input is refused rather than producing a broken character. */
	ok(!hero_create(&h, FLAVOR_DIABLO, 99, "Aidan", err), "an impossible class is refused");
	ok(!hero_create(&h, FLAVOR_DIABLO, PC_WARRIOR, "", err), "an empty name is refused");
	ok(!hero_create(&h, FLAVOR_DIABLO, PC_WARRIOR, "a/b", err), "a bad name is refused");

	/* Levelling a generated character is the same path as levelling any other. */
	ok(hero_create(&h, FLAVOR_HELLFIRE, 3, "Jazreth", err), "a fresh Monk");
	int gained = hero_level_up(&h, FLAVOR_HELLFIRE, 30, err);
	ok(gained == 29 && h.pLevel == 30, "raised to 30");
	ok(h.pStatPts == 29 * 5, "  ...with the points those levels grant");
	ok(hero_validate(&h, FLAVOR_HELLFIRE, err), "  ...still valid");

	/* And gold, which a new save can always take -- no game, so no stacks to
	 * clone from and none needed. */
	ok(hero_set_gold(&h, FLAVOR_HELLFIRE, 50000, err), "and 50000 gold");
	ok(hero_gold_in_stacks(&h) == 50000, "  ...in real stacks");
	ok(hero_validate(&h, FLAVOR_HELLFIRE, err), "  ...still valid");

	/* Round-trip through an actual archive. */
	const char *p = tmppath("made.hsv");
	remove(p);
	ok(save_create(p, &h, err), "written to a new save");
	PkPlayerStruct back;
	ok(save_read_hero(p, &back, NULL, err), "read back");
	ok(memcmp(&back, &h, sizeof(back)) == 0, "byte-identical");
	ok(!save_has_game(p), "with no game in progress, as a new character has none");
	ok(!save_create(p, &h, err), "creating over an existing save is refused");
	remove(p);
}

/* ------------------------------------------------------------------ */
/* 1. Gold distribution                                                */
/* ------------------------------------------------------------------ */

static void check_gold(void)
{
	section("1. gold distribution");

	char err[HERO_ERR_LEN];
	char why[256];

	static const struct {
		int amount;
		int stacks;
	} cases[] = {
		{ 0, 0 }, { 1, 1 }, { 4999, 1 }, { 5000, 1 }, { 5001, 2 },
		{ 10000, 2 }, { 12345, 3 }, { 199999, 40 }, { 200000, 40 },
	};

	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		PkPlayerStruct h;
		base_hero(&h, PC_WARRIOR);

		if (!hero_set_gold(&h, FLAVOR_DIABLO, cases[c].amount, err)) {
			okf(0, "set gold to %d", cases[c].amount);
			printf("        %s\n", err);
			continue;
		}

		int stacks = 0;
		for (int i = 0; i < h._pNumInv; i++)
			if (h.InvList[i].idx == IDI_GOLD)
				stacks++;

		okf(h.pGold == cases[c].amount
		        && hero_gold_in_stacks(&h) == cases[c].amount
		        && stacks == cases[c].stacks
		        && inv_consistent(&h, why, sizeof(why)),
		    "gold %d -> %d stack%s, cached and stacked totals agree",
		    cases[c].amount, cases[c].stacks, cases[c].stacks == 1 ? "" : "s");
	}

	/*
	 * Hellfire allows twice as much per stack. ValidatePlayer does
	 * `maxGold = GOLD_MAX_LIMIT; if (gbIsHellfire) maxGold *= 2;` and clamps
	 * each stack to it every tick, so using the Diablo figure for both halved
	 * what a Hellfire character could be given.
	 */
	{
		ok(hero_gold_stack_max(FLAVOR_DIABLO) == GOLD_MAX_LIMIT,
		    "a Diablo stack holds GOLD_MAX_LIMIT");
		ok(hero_gold_stack_max(FLAVOR_HELLFIRE) == GOLD_MAX_LIMIT * 2,
		    "a Hellfire stack holds twice that");

		PkPlayerStruct d, hf;
		base_hero(&d, PC_WARRIOR);
		base_hero(&hf, PC_WARRIOR);
		ok(hero_gold_capacity(&hf, FLAVOR_HELLFIRE)
		        == 2 * hero_gold_capacity(&d, FLAVOR_DIABLO),
		    "  ...so a Hellfire character has twice the capacity");

		/* An amount Diablo cannot hold, that Hellfire can. */
		int over = NUM_INV_GRID_ELEM * GOLD_MAX_LIMIT + 1;
		ok(!hero_set_gold(&d, FLAVOR_DIABLO, over, err),
		    "an amount past Diablo's capacity is refused");
		ok(hero_set_gold(&hf, FLAVOR_HELLFIRE, over, err),
		    "  ...and accepted for Hellfire");
		ok(hero_gold_in_stacks(&hf) == over, "  ...totalling correctly");

		int biggest = 0;
		for (int i = 0; i < hf._pNumInv; i++)
			if (hf.InvList[i].idx == IDI_GOLD && hf.InvList[i].wValue > biggest)
				biggest = hf.InvList[i].wValue;
		ok(biggest <= GOLD_MAX_LIMIT * 2, "  ...with no stack over the Hellfire limit");
	}

	/* Over capacity on an empty inventory. */
	{
		PkPlayerStruct h;
		base_hero(&h, PC_WARRIOR);
		ok(!hero_set_gold(&h, FLAVOR_DIABLO, 200001, err), "200001 gold is refused (40 cells x 5000)");
		printf("        %s\n", err);
		ok(h.pGold == 0 && h._pNumInv == 0, "the character was left unmodified");
	}

	/* No stack may exceed the game's per-pile limit. */
	{
		PkPlayerStruct h;
		base_hero(&h, PC_WARRIOR);
		hero_set_gold(&h, FLAVOR_DIABLO, 123456, err);
		int over = 0;
		for (int i = 0; i < h._pNumInv; i++)
			if (h.InvList[i].idx == IDI_GOLD && h.InvList[i].wValue > GOLD_MAX_LIMIT)
				over++;
		ok(over == 0, "no stack exceeds GOLD_MAX_LIMIT (5000)");

		int distinct = 1;
		for (int i = 0; i < h._pNumInv && distinct; i++)
			for (int j = i + 1; j < h._pNumInv; j++)
				if (h.InvList[i].idx == IDI_GOLD && h.InvList[j].idx == IDI_GOLD
				    && h.InvList[i].iSeed == h.InvList[j].iSeed)
					distinct = 0;
		ok(distinct, "gold stacks get distinct seeds (VerifyGoldSeeds re-rolls dups)");
	}

	/* Existing items must survive; existing gold must be replaced, not added to. */
	{
		PkPlayerStruct h;
		base_hero(&h, PC_WARRIOR);
		add_item(&h, 0, 0, IDI_ROGUE);
		add_item(&h, 1, 1, IDI_SORCEROR);
		hero_set_gold(&h, FLAVOR_DIABLO, 7000, err);

		int before_items = 0;
		for (int i = 0; i < h._pNumInv; i++)
			if (h.InvList[i].idx != IDI_GOLD && h.InvList[i].idx != 0xFFFF)
				before_items++;

		ok(hero_set_gold(&h, FLAVOR_DIABLO, 3000, err), "re-setting gold with items present");

		int after_items = 0, gold_stacks = 0;
		for (int i = 0; i < h._pNumInv; i++) {
			if (h.InvList[i].idx == IDI_GOLD)
				gold_stacks++;
			else if (h.InvList[i].idx != 0xFFFF)
				after_items++;
		}
		okf(after_items == before_items, "non-gold items preserved (%d)", before_items);
		ok(gold_stacks == 1, "old gold stacks replaced, not accumulated");
		ok(hero_gold_in_stacks(&h) == 3000 && h.pGold == 3000, "total is exactly 3000");
		ok(inv_consistent(&h, why, sizeof(why)), "inventory still consistent");
		if (!inv_consistent(&h, why, sizeof(why)))
			printf("        %s\n", why);
	}

	/* Capacity shrinks as items take cells. */
	{
		PkPlayerStruct h;
		base_hero(&h, PC_WARRIOR);
		for (int i = 0; i < 10; i++)
			add_item(&h, i, i, IDI_ROGUE);
		ok(hero_free_inv_cells(&h) == 30, "30 cells free with 10 items");
		ok(hero_gold_capacity(&h, FLAVOR_DIABLO) == 150000, "capacity reflects free cells");
		ok(!hero_set_gold(&h, FLAVOR_DIABLO, 150001, err), "over the reduced capacity is refused");
		ok(hero_set_gold(&h, FLAVOR_DIABLO, 150000, err), "exactly the capacity is accepted");
		ok(inv_consistent(&h, why, sizeof(why)), "inventory consistent at capacity");
	}

	/* Setting gold to zero clears every stack. */
	{
		PkPlayerStruct h;
		base_hero(&h, PC_WARRIOR);
		hero_set_gold(&h, FLAVOR_DIABLO, 50000, err);
		ok(hero_set_gold(&h, FLAVOR_DIABLO, 0, err), "gold back to zero");
		ok(hero_gold_in_stacks(&h) == 0 && h.pGold == 0 && h._pNumInv == 0,
		    "all stacks removed");
		ok(hero_free_inv_cells(&h) == NUM_INV_GRID_ELEM, "all cells free again");
	}

	/* Deterministic: same input, same bytes. */
	{
		PkPlayerStruct a, b;
		base_hero(&a, PC_ROGUE);
		base_hero(&b, PC_ROGUE);
		hero_set_gold(&a, FLAVOR_DIABLO, 33333, err);
		hero_set_gold(&b, FLAVOR_DIABLO, 33333, err);
		ok(memcmp(&a, &b, sizeof(a)) == 0, "gold layout is deterministic");
	}

	/* Negative is refused. */
	{
		PkPlayerStruct h;
		base_hero(&h, PC_WARRIOR);
		ok(!hero_set_gold(&h, FLAVOR_DIABLO, -1, err), "negative gold refused");
	}
}

/* ------------------------------------------------------------------ */
/* 2. Validation                                                       */
/* ------------------------------------------------------------------ */

static void check_validation(void)
{
	section("2. validation against the game's limits");

	char err[HERO_ERR_LEN];
	PkPlayerStruct h;

	base_hero(&h, PC_WARRIOR);
	ok(hero_validate(&h, FLAVOR_DIABLO, err), "a plain character validates");

	/* Per-class stat caps, straight from MaxStats. */
	static const struct {
		int cls;
		int attr;
		const char *label;
	} caps[] = {
		{ PC_WARRIOR, ATTRIB_STR, "warrior strength" },
		{ PC_WARRIOR, ATTRIB_MAG, "warrior magic" },
		{ PC_ROGUE, ATTRIB_DEX, "rogue dexterity" },
		{ PC_SORCERER, ATTRIB_MAG, "sorcerer magic" },
		{ PC_SORCERER, ATTRIB_STR, "sorcerer strength" },
	};
	for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
		int cap = hero_max_stat(FLAVOR_DIABLO, caps[i].cls, caps[i].attr);
		base_hero(&h, caps[i].cls);
		BYTE *field = (caps[i].attr == ATTRIB_STR) ? &h.pBaseStr
		    : (caps[i].attr == ATTRIB_MAG)         ? &h.pBaseMag
		    : (caps[i].attr == ATTRIB_DEX)         ? &h.pBaseDex
		                                           : &h.pBaseVit;
		*field = (BYTE)cap;
		int at_cap = hero_validate(&h, FLAVOR_DIABLO, err);
		int over = 1;
		if (cap < 255) {
			*field = (BYTE)(cap + 1);
			over = !hero_validate(&h, FLAVOR_DIABLO, err);
		}
		okf(at_cap && over, "%s cap is %d, %d rejected", caps[i].label, cap, cap + 1);
	}

	base_hero(&h, PC_WARRIOR);
	h.pLevel = 0;
	ok(!hero_validate(&h, FLAVOR_DIABLO, err), "level 0 rejected");
	h.pLevel = 51;
	ok(!hero_validate(&h, FLAVOR_DIABLO, err), "level 51 rejected");
	h.pLevel = 50;
	ok(hero_validate(&h, FLAVOR_DIABLO, err), "level 50 accepted");
	h.pLevel = 1;
	ok(hero_validate(&h, FLAVOR_DIABLO, err), "level 1 accepted");

	base_hero(&h, PC_WARRIOR);
	h.pClass = 7;
	ok(!hero_validate(&h, FLAVOR_DIABLO, err), "unknown class rejected");

	base_hero(&h, PC_WARRIOR);
	h.pExperience = -1;
	ok(!hero_validate(&h, FLAVOR_DIABLO, err), "negative experience rejected");

	base_hero(&h, PC_WARRIOR);
	h.pHPBase = h.pMaxHPBase + 64;
	ok(!hero_validate(&h, FLAVOR_DIABLO, err), "life above maximum rejected");

	base_hero(&h, PC_WARRIOR);
	h.pHPBase = 0;
	ok(!hero_validate(&h, FLAVOR_DIABLO, err), "life below 1 rejected (the game rewrites it on load)");

	base_hero(&h, PC_WARRIOR);
	h.plrlevel = NUMLEVELS;
	ok(!hero_validate(&h, FLAVOR_DIABLO, err), "dungeon level beyond the last is rejected");

	base_hero(&h, PC_WARRIOR);
	h._pNumInv = NUM_INV_GRID_ELEM + 1;
	ok(!hero_validate(&h, FLAVOR_DIABLO, err), "_pNumInv beyond the grid rejected");

	/* Names. */
	ok(!hero_name_valid("", err), "empty name rejected");
	ok(hero_name_valid("Aidan", err), "ordinary name accepted");
	ok(!hero_name_valid("a/b", err), "name with '/' rejected");
	ok(!hero_name_valid("a\\b", err), "name with '\\' rejected");
	ok(!hero_name_valid("a\tb", err), "name with a control character rejected");
	{
		char long_name[PLR_NAME_LEN + 8];
		memset(long_name, 'x', sizeof(long_name) - 1);
		long_name[sizeof(long_name) - 1] = '\0';
		ok(!hero_name_valid(long_name, err), "over-long name rejected");
		long_name[PLR_NAME_LEN - 1] = '\0';
		ok(hero_name_valid(long_name, err), "name of exactly the limit accepted");
	}
}

/* ------------------------------------------------------------------ */
/* 3. Spells                                                           */
/* ------------------------------------------------------------------ */

static void check_spells(void)
{
	section("3. spell lookup");

	int fb = hero_find_spell(FLAVOR_DIABLO, "Firebolt");
	ok(fb > 0, "\"Firebolt\" resolves");
	ok(hero_find_spell(FLAVOR_DIABLO, "firebolt") == fb, "lookup is case-insensitive");
	ok(hero_find_spell(FLAVOR_DIABLO, "FIRE BOLT") == fb, "spaces are ignored");
	ok(hero_find_spell(FLAVOR_DIABLO, "fire-bolt") == fb, "hyphens are ignored");

	int cl = hero_find_spell(FLAVOR_DIABLO, "Chain Lightning");
	ok(cl > 0 && cl != fb, "\"Chain Lightning\" resolves distinctly");
	ok(hero_find_spell(FLAVOR_DIABLO, "chainlightning") == cl, "multi-word names match unspaced");

	ok(hero_find_spell(FLAVOR_DIABLO, "Nonexistent Spell") == -1, "unknown name rejected");
	ok(hero_find_spell(FLAVOR_DIABLO, "") == -1, "empty name rejected");
	ok(hero_find_spell(FLAVOR_DIABLO, "3") == 3, "a bare number is taken as a spell id");
	ok(hero_find_spell(FLAVOR_DIABLO, "0") == -1, "spell id 0 (SPL_NULL) rejected");
	ok(hero_find_spell(FLAVOR_DIABLO, "999") == -1, "out-of-range id rejected");

	ok(hero_spell_name(FLAVOR_DIABLO, fb) != NULL && strcmp(hero_spell_name(FLAVOR_DIABLO, fb), "Firebolt") == 0,
	    "names come from the game's own spelldata table");

	/* pMemSpells is a bitmask: SPELLBIT(s) == 1 << (s-1). */
	PkPlayerStruct h;
	base_hero(&h, PC_SORCERER);
	h.pMemSpells |= SPELLBIT(fb);
	ok((h.pMemSpells & SPELLBIT(fb)) != 0, "spell bit sets");
	ok((h.pMemSpells & SPELLBIT(cl)) == 0, "other bits untouched");
	ok(h.pMemSpells == ((unsigned long long)1 << (fb - 1)), "bit position is 1 << (id-1)");
}

/* ------------------------------------------------------------------ */
/* 4. Experience and levels                                            */
/* ------------------------------------------------------------------ */

static void check_exp(void)
{
	section("4. experience and levels");

	ok(hero_max_level() == 50, "level cap is 50 (MAXCHARLEVEL - 1)");
	ok(hero_exp_for_level(1) == 0, "level 1 needs no experience");
	ok(hero_exp_for_level(2) == 2000, "level 2 needs 2000");

	/* Round-trip: the exp for level L must compute back to level L. */
	int bad = 0;
	for (int l = 1; l <= hero_max_level(); l++) {
		int e = hero_exp_for_level(l);
		if (hero_level_for_exp(e) != l) {
			printf("        level %d: exp %d maps back to %d\n", l, e,
			    hero_level_for_exp(e));
			bad++;
		}
	}
	okf(bad == 0, "exp<->level round-trips for all %d levels", hero_max_level());

	ok(hero_level_for_exp(0) == 1, "zero experience is level 1");
	ok(hero_level_for_exp(1999) == 1, "just under the threshold stays level 1");
	ok(hero_level_for_exp(2000) == 2, "exactly the threshold advances");
	ok(hero_level_for_exp(2000000000) == 50, "huge experience caps at 50");

	/*
	 * AddPlrExperience recomputes the level from experience starting at 0
	 * (Source/player.cpp:1074). It never lowers a level, but a character
	 * whose experience is short will not gain one either -- the usual way an
	 * edited character ends up stuck, so it must warn.
	 */
	PkPlayerStruct h;

	g_warns = 0;
	base_hero(&h, PC_WARRIOR);
	h.pLevel = 50;
	h.pExperience = 0;
	hero_warnings(&h, FLAVOR_DIABLO, count_warn);
	ok(g_warns > 0, "level 50 with no experience warns");

	g_warns = 0;
	base_hero(&h, PC_WARRIOR);
	h.pLevel = 1;
	h.pExperience = hero_exp_for_level(30);
	hero_warnings(&h, FLAVOR_DIABLO, count_warn);
	ok(g_warns > 0, "level 1 with level-30 experience warns");

	g_warns = 0;
	base_hero(&h, PC_WARRIOR);
	hero_warnings(&h, FLAVOR_DIABLO, count_warn);
	ok(g_warns == 0, "a consistent character produces no warnings");

	g_warns = 0;
	base_hero(&h, PC_WARRIOR);
	h.pGold = 5000; /* cached, but no stacks */
	hero_warnings(&h, FLAVOR_DIABLO, count_warn);
	ok(g_warns > 0, "cached gold with no stacks warns");
}

/* ------------------------------------------------------------------ */
/* 5. Fixed point                                                      */
/* ------------------------------------------------------------------ */

static void check_fixed_point(void)
{
	section("5. fixed-point life and mana");

	ok(HERO_FROM_WHOLE(1) == 64, "1 point is 64 raw units");
	ok(HERO_TO_WHOLE(64) == 1, "64 raw units is 1 point");
	ok(HERO_TO_WHOLE(HERO_FROM_WHOLE(137)) == 137, "conversion round-trips");
	ok(HERO_TO_WHOLE(127) == 1, "fractional remainder is truncated, not rounded");
}

/* ------------------------------------------------------------------ */
/* 6. End to end through a real archive                                */
/* ------------------------------------------------------------------ */

typedef struct Entry {
	const char *name;
	const BYTE *data;
	DWORD len;
} Entry;

static int build_archive(const char *path, const Entry *entries, int count)
{
	_FILEHEADER hdr;
	_BLOCKENTRY *block = (_BLOCKENTRY *)calloc(MPQ_INDEX_ENTRIES, sizeof(_BLOCKENTRY));
	_HASHENTRY *hash = (_HASHENTRY *)malloc(MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
	BYTE *data = NULL;
	DWORD data_len = 0;

	memset(hash, 0xFF, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
	InitHash();

	for (int e = 0; e < count; e++) {
		DWORD file_len = entries[e].len;
		DWORD nsec = (file_len + MPQ_SECTOR_SIZE - 1) / MPQ_SECTOR_SIZE;
		DWORD tbl = (nsec + 1) * sizeof(DWORD);
		BYTE *body = (BYTE *)malloc(tbl + nsec * MPQ_SECTOR_SIZE * 2);
		DWORD *offs = (DWORD *)body;
		DWORD pos = tbl;

		for (DWORD s = 0; s < nsec; s++) {
			DWORD raw = file_len - s * MPQ_SECTOR_SIZE;
			if (raw > MPQ_SECTOR_SIZE)
				raw = MPQ_SECTOR_SIZE;
			BYTE sector[MPQ_SECTOR_SIZE];
			memcpy(sector, entries[e].data + s * MPQ_SECTOR_SIZE, raw);
			int stored = PkwareCompress(sector, (int)raw);
			offs[s] = pos;
			memcpy(body + pos, sector, (size_t)stored);
			pos += (DWORD)stored;
		}
		offs[nsec] = pos;

		block[e].offset = (int)(MPQ_DATA_OFFSET + data_len);
		block[e].sizealloc = (int)pos;
		block[e].sizefile = (int)file_len;
		block[e].flags = (int)(MPQ_FLAG_EXISTS | MPQ_FLAG_IMPLODE);

		data = (BYTE *)realloc(data, data_len + pos);
		memcpy(data + data_len, body, pos);
		data_len += pos;
		free(body);

		DWORD idx = Hash(entries[e].name, 0) & 0x7FF;
		while (hash[idx].block != -1)
			idx = (idx + 1) & 0x7FF;
		hash[idx].hashcheck[0] = (int)Hash(entries[e].name, 1);
		hash[idx].hashcheck[1] = (int)Hash(entries[e].name, 2);
		hash[idx].lcid = 0;
		hash[idx].block = e;
	}

	memset(&hdr, 0, sizeof(hdr));
	hdr.signature = (int)MPQ_SIGNATURE;
	hdr.headersize = 32;
	hdr.filesize = (int)(MPQ_DATA_OFFSET + data_len);
	hdr.sectorsizeid = 3;
	hdr.hashoffset = MPQ_HASH_OFFSET;
	hdr.blockoffset = MPQ_BLOCK_OFFSET;
	hdr.hashcount = MPQ_INDEX_ENTRIES;
	hdr.blockcount = MPQ_INDEX_ENTRIES;

	Encrypt((DWORD *)block, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY), Hash("(block table)", 3));
	Encrypt((DWORD *)hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), Hash("(hash table)", 3));

	FILE *f = fopen(path, "wb");
	int rc = -1;
	if (f != NULL) {
		fwrite(&hdr, sizeof(hdr), 1, f);
		fwrite(block, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY), 1, f);
		fwrite(hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), 1, f);
		if (data_len)
			fwrite(data, data_len, 1, f);
		fclose(f);
		rc = 0;
	}
	free(block);
	free(hash);
	free(data);
	return rc;
}

static void check_end_to_end(void)
{
	section("6. edit -> write -> read back");

	PkPlayerStruct plain;
	base_hero(&plain, PC_SORCERER);
	strcpy(plain.pName, "Jazreth");

	BYTE blob[1288];
	memset(blob, 0, sizeof(blob));
	memcpy(blob, &plain, sizeof(plain));
	codec_encode(blob, sizeof(plain), 1288, SAVE_PASSWORD_SINGLE);

	Entry e = { "hero", blob, 1288 };
	const char *path = tmppath("single_0.sv");
	ok(build_archive(path, &e, 1) == 0, "build a save to edit");

	char err[MPQ_ERR_LEN];
	PkPlayerStruct h;
	ok(save_read_hero(path, &h, NULL, err), "read it");

	/* A realistic set of edits. */
	memset(h.pName, 0, PLR_NAME_LEN);
	strcpy(h.pName, "Jazreth II");
	h.pLevel = 30;
	h.pExperience = hero_exp_for_level(30);
	h.pBaseMag = 200;
	h.pMaxHPBase = HERO_FROM_WHOLE(120);
	h.pHPBase = HERO_FROM_WHOLE(120);
	int sid = hero_find_spell(FLAVOR_DIABLO, "Fireball");
	ok(hero_set_spell_level(&h, FLAVOR_DIABLO, sid, 12, err), "set Fireball to level 12");
	ok(hero_set_gold(&h, FLAVOR_DIABLO, 87654, err), "set gold to 87654");

	char verr[HERO_ERR_LEN];
	ok(hero_validate(&h, FLAVOR_DIABLO, verr), "edited character validates");
	if (!hero_validate(&h, FLAVOR_DIABLO, verr))
		printf("        %s\n", verr);

	ok(save_write_hero(path, &h, err), "write it back");

	PkPlayerStruct back;
	ok(save_read_hero(path, &back, NULL, err), "read it back");
	ok(memcmp(&back, &h, sizeof(back)) == 0, "round-trips exactly");
	ok(strcmp(back.pName, "Jazreth II") == 0, "name persisted");
	ok(back.pLevel == 30 && back.pBaseMag == 200, "level and magic persisted");
	ok(back.pSplLvl[sid] == 12 && (back.pMemSpells & SPELLBIT(sid)) != 0,
	    "spell level and book entry persisted");
	ok(back.pGold == 87654 && hero_gold_in_stacks(&back) == 87654,
	    "gold persisted, cached and stacked agree");

	char why[256];
	ok(inv_consistent(&back, why, sizeof(why)), "inventory consistent after the round trip");

	g_warns = 0;
	hero_warnings(&back, FLAVOR_DIABLO, count_warn);
	ok(g_warns == 0, "the edited character produces no warnings");
}


/* ------------------------------------------------------------------ */
/* 7. Hellfire                                                         */
/* ------------------------------------------------------------------ */

/*
 * Diablo and Hellfire share the packed format byte for byte, so nothing in
 * the MPQ or codec layers is flavor-aware. What must differ is meaning: three
 * more classes with their own caps, ten more persisted spells living in
 * pSplLvl2, and eight more dungeon levels.
 */
static void check_hellfire(void)
{
	section("7. Hellfire");

	char err[HERO_ERR_LEN];

	/* Flavor is inferred from the extension. */
	ok(hero_flavor_for_path("single_0.hsv") == FLAVOR_HELLFIRE, ".hsv is Hellfire");
	ok(hero_flavor_for_path("single_0.sv") == FLAVOR_DIABLO, ".sv is Diablo");
	ok(hero_flavor_for_path("/a/b/SINGLE_3.HSV") == FLAVOR_HELLFIRE,
	    "extension match is case-insensitive");
	ok(hero_flavor_for_path("hero.bin") == FLAVOR_DIABLO, "anything else is Diablo");

	ok(hero_num_classes(FLAVOR_DIABLO) == 3, "Diablo has 3 classes");
	ok(hero_num_classes(FLAVOR_HELLFIRE) == 6, "Hellfire has 6 classes");
	ok(hero_num_levels(FLAVOR_DIABLO) == 17, "Diablo has 17 dungeon levels");
	ok(hero_num_levels(FLAVOR_HELLFIRE) == 25, "Hellfire has 25");
	ok(hero_spell_slots(FLAVOR_DIABLO) == 37, "Diablo defines 37 spells");
	ok(hero_spell_slots(FLAVOR_HELLFIRE) == 52, "Hellfire defines 52");

	/* The three Hellfire classes, with the caps from MaxStats. */
	static const struct {
		int cls;
		const char *name;
		int str, mag, dex, vit;
	} hf[] = {
		{ 3, "Monk", 150, 80, 150, 80 },
		{ 4, "Bard", 120, 120, 120, 100 },
		{ 5, "Barbarian", 255, 0, 55, 150 },
	};
	for (size_t i = 0; i < sizeof(hf) / sizeof(hf[0]); i++) {
		okf(strcmp(hero_class_name(FLAVOR_HELLFIRE, hf[i].cls), hf[i].name) == 0,
		    "class %d is %s", hf[i].cls, hf[i].name);
		okf(hero_max_stat(FLAVOR_HELLFIRE, hf[i].cls, ATTRIB_STR) == hf[i].str
		        && hero_max_stat(FLAVOR_HELLFIRE, hf[i].cls, ATTRIB_MAG) == hf[i].mag
		        && hero_max_stat(FLAVOR_HELLFIRE, hf[i].cls, ATTRIB_DEX) == hf[i].dex
		        && hero_max_stat(FLAVOR_HELLFIRE, hf[i].cls, ATTRIB_VIT) == hf[i].vit,
		    "%s caps are %d/%d/%d/%d", hf[i].name, hf[i].str, hf[i].mag,
		    hf[i].dex, hf[i].vit);
		ok(strcmp(hero_class_name(FLAVOR_DIABLO, hf[i].cls), "unknown") == 0,
		    "  ...and does not exist in Diablo");
	}

	/* A Monk validates as Hellfire and is rejected as Diablo. */
	PkPlayerStruct h;
	base_hero(&h, 3); /* Monk */
	h.pBaseStr = 150;
	ok(hero_validate(&h, FLAVOR_HELLFIRE, err), "a Monk at its cap validates as Hellfire");
	ok(!hero_validate(&h, FLAVOR_DIABLO, err), "the same Monk is rejected as Diablo");
	printf("        %s\n", err);

	/*
	 * Reading one as Diablo must say so rather than silently misreport. This
	 * is an error, not a warning: every stat cap shown would be for the wrong
	 * game, so importing it should be refused.
	 */
	{
		char cerr[HERO_ERR_LEN] = { 0 };
		ok(!hero_validate(&h, FLAVOR_DIABLO, cerr)
		        && strstr(cerr, "--hellfire") != NULL,
		    "a Hellfire class read as Diablo is an error naming the fix");
	}

	/* Dungeon levels beyond Diablo's 16. */
	base_hero(&h, 3);
	h.plrlevel = 20;
	ok(hero_validate(&h, FLAVOR_HELLFIRE, err), "dungeon level 20 is fine in Hellfire");
	base_hero(&h, PC_WARRIOR);
	h.plrlevel = 20;
	ok(!hero_validate(&h, FLAVOR_DIABLO, err), "dungeon level 20 is rejected in Diablo");

	/* Hellfire-only spell names resolve, and only from the Hellfire table. */
	int apoc = hero_find_spell(FLAVOR_HELLFIRE, "Apocalypse");
	ok(apoc > 0, "\"Apocalypse\" resolves in Hellfire");
	ok(hero_find_spell(FLAVOR_DIABLO, "Search") == -1,
	    "a Hellfire-only spell does not resolve in Diablo");
	int firebolt_d = hero_find_spell(FLAVOR_DIABLO, "Firebolt");
	int firebolt_h = hero_find_spell(FLAVOR_HELLFIRE, "Firebolt");
	ok(firebolt_d == firebolt_h && firebolt_d > 0,
	    "shared spells keep the same id in both");

	/*
	 * Spells 37..46 live in pSplLvl2, not pSplLvl. Getting this wrong would
	 * write into the wrong array and corrupt neighbouring fields.
	 */
	base_hero(&h, 3);
	/*
	 * It has to be one with a book, or there would be no book bit to check:
	 * the first few Hellfire spells (Mana, the Magi, the Jester) are staff-only
	 * and sBookLvl is -1 for them.
	 */
	int hi = -1;
	for (int sp = MAX_SPELLS; sp < hero_spell_persisted(); sp++) {
		if (hero_spell_has_book(FLAVOR_HELLFIRE, sp)) {
			hi = sp;
			break;
		}
	}
	okf(hi >= MAX_SPELLS, "found a book spell in the pSplLvl2 range (id %d, %s)", hi,
	    hi > 0 ? hero_spell_name(FLAVOR_HELLFIRE, hi) : "?");
	if (hi >= MAX_SPELLS) {
		PkPlayerStruct before = h;
		ok(hero_set_spell_level(&h, FLAVOR_HELLFIRE, hi, 9, err), "set a pSplLvl2 spell");
		ok(hero_get_spell_level(&h, hi) == 9, "reads back through pSplLvl2");
		ok(h.pSplLvl2[hi - MAX_SPELLS] == 9, "landed in pSplLvl2, not pSplLvl");
		ok(memcmp(h.pSplLvl, before.pSplLvl, MAX_SPELLS) == 0,
		    "the pSplLvl array was not touched");
		ok((h.pMemSpells & SPELLBIT(hi)) != 0, "spell book bit set");
		ok(hero_set_spell_level(&h, FLAVOR_HELLFIRE, hi, 0, err) && (h.pMemSpells & SPELLBIT(hi)) == 0,
		    "level 0 clears the book bit");
	}

	/* A staff-only spell gets a level but never a book bit. */
	base_hero(&h, 3);
	int staff_only = hero_find_spell(FLAVOR_HELLFIRE, "Mana");
	okf(staff_only > 0 && !hero_spell_has_book(FLAVOR_HELLFIRE, staff_only),
	    "Mana (id %d) is staff-only", staff_only);
	ok(hero_set_spell_level(&h, FLAVOR_HELLFIRE, staff_only, 6, err),
	    "it can still be given a level");
	ok((h.pMemSpells & SPELLBIT(staff_only)) == 0,
	    "  ...but is kept out of the spell book, which the game would mask anyway");

	/* Ids 47..51 exist in Hellfire but no save can carry them. */
	base_hero(&h, 3);
	ok(!hero_set_spell_level(&h, FLAVOR_HELLFIRE, hero_spell_persisted(), 5, err),
	    "a spell id beyond pSplLvl2 is refused rather than silently dropped");
	printf("        %s\n", err);
	ok(hero_spell_persisted() == 47, "ids 0..46 are the persistable range");

	/* Round-trip a Hellfire character through a save. */
	base_hero(&h, 5); /* Barbarian */
	memset(h.pName, 0, PLR_NAME_LEN);
	strcpy(h.pName, "Grognak");
	h.pBaseStr = 255;
	h.pBaseMag = 0;
	h.plrlevel = 22;
	if (hi >= MAX_SPELLS)
		hero_set_spell_level(&h, FLAVOR_HELLFIRE, hi, 4, err);
	ok(hero_validate(&h, FLAVOR_HELLFIRE, err), "a Barbarian validates");
	if (!hero_validate(&h, FLAVOR_HELLFIRE, err))
		printf("        %s\n", err);

	BYTE blob[1288];
	memset(blob, 0, sizeof(blob));
	memcpy(blob, &h, sizeof(h));
	codec_encode(blob, sizeof(h), 1288, SAVE_PASSWORD_SINGLE);
	Entry e = { "hero", blob, 1288 };
	const char *path = tmppath("single_0.hsv");
	ok(build_archive(path, &e, 1) == 0, "build a Hellfire save");

	PkPlayerStruct back;
	char merr[MPQ_ERR_LEN];
	ok(save_read_hero(path, &back, NULL, merr), "read it back");
	ok(memcmp(&back, &h, sizeof(back)) == 0, "Hellfire character round-trips exactly");
	ok(hero_flavor_for_path(path) == FLAVOR_HELLFIRE, "and is detected as Hellfire");

	h.pBaseVit = 120;
	ok(save_write_hero(path, &h, merr), "write an edit back");
	ok(save_read_hero(path, &back, NULL, merr) && back.pBaseVit == 120,
	    "the edit persisted");
}

/* ------------------------------------------------------------------ */

int main(void)
{
	g_tmpdir = getenv("BUTCHER_TMPDIR");
	if (g_tmpdir == NULL) {
		/* Run directly rather than through `make check`: keep scratch files
		 * out of the source tree. */
		g_tmpdir = "build/test-scratch";
		mkdir(g_tmpdir, 0755);
	}

	printf("Devilution character editor -- Phase 4 editor\n");
	printf("scratch dir: %s\n", g_tmpdir);

	check_create();
	check_gold();
	check_validation();
	check_spells();
	check_exp();
	check_fixed_point();
	check_end_to_end();
	check_hellfire();

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
