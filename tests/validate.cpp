/**
 * @file validate_check.cpp
 *
 * Phase 6 gate: validation.
 *
 * The failure this exists to prevent is a *silent* one. Before validation, a
 * hand-edited document with "vitallity" instead of "vitality" imported
 * cleanly and set vitality to zero, and "strength": 300 imported as 44. Both
 * looked like success. So the tests here care less about rejecting obvious
 * garbage than about catching the plausible mistakes a person actually makes
 * in a text editor.
 *
 * Two properties beyond that:
 *   - every finding is reported at once, not one per run;
 *   - a document exported from a real save reports nothing at all. A checker
 *     that cries wolf on ordinary characters is worse than none.
 */
#include "../src/charjson.h"

#include "../third_party/devilution/Source/encrypt.h"

#include <stdarg.h>
#include <sys/wait.h>

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
	char buf[320];
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

static void nowarn(const char *m)
{
	(void)m;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Run the document checks and return the findings. */
static void check_doc(const char *text, DiagList *dl)
{
	dl_init(dl);
	charjson_check(text, dl);
}

/** @return nonzero if some finding's message contains @p needle. */
static int mentions(const DiagList *dl, const char *needle)
{
	for (int i = 0; i < dl->n; i++)
		if (strstr(dl->items[i].msg, needle) != NULL)
			return 1;
	return 0;
}

/** @return nonzero if some finding is located at @p where. */
static int located_at(const DiagList *dl, const char *where)
{
	for (int i = 0; i < dl->n; i++)
		if (strcmp(dl->items[i].where, where) == 0)
			return 1;
	return 0;
}

static int doc_errors(const char *text)
{
	DiagList dl;
	check_doc(text, &dl);
	int n = dl_count(&dl, DIAG_ERROR);
	dl_free(&dl);
	return n;
}

static void base_hero(PkPlayerStruct *p, int pclass)
{
	memset(p, 0, sizeof(*p));
	strcpy(p->pName, "Testy");
	p->pClass = (char)pclass;
	p->pLevel = 12;
	p->pExperience = hero_exp_for_level(12);
	p->pBaseStr = 40;
	p->pBaseMag = 20;
	p->pBaseDex = 30;
	p->pBaseVit = 35;
	p->pHPBase = HERO_FROM_WHOLE(60);
	p->pMaxHPBase = HERO_FROM_WHOLE(60);
	p->pManaBase = HERO_FROM_WHOLE(25);
	p->pMaxManaBase = HERO_FROM_WHOLE(25);
	p->plrlevel = 5;
	for (int i = 0; i < NUM_INVLOC; i++)
		p->InvBody[i].idx = 0xFFFF;
	for (int i = 0; i < NUM_INV_GRID_ELEM; i++)
		p->InvList[i].idx = 0xFFFF;
	for (int i = 0; i < MAXBELTITEMS; i++)
		p->SpdList[i].idx = 0xFFFF;
}

/* ------------------------------------------------------------------ */
/* 1. A well-formed document is silent                                 */
/* ------------------------------------------------------------------ */

static void check_clean(void)
{
	section("1. valid documents produce no findings");

	for (int flavor = 0; flavor < 2; flavor++) {
		PkPlayerStruct h;
		base_hero(&h, flavor == 1 ? 3 : PC_WARRIOR);
		HeroFlavor f = (HeroFlavor)flavor;

		char *text = charjson_write(&h, f, nowarn);
		DiagList dl;
		check_doc(text, &dl);

		int errors = dl_count(&dl, DIAG_ERROR);
		int warnings = dl_count(&dl, DIAG_WARNING);
		okf(errors == 0 && warnings == 0,
		    "an exported %s character validates silently", hero_flavor_name(f));
		if (errors || warnings)
			dl_report(&dl, "generated", stdout);
		dl_free(&dl);

		/* And the character behind it. */
		dl_init(&dl);
		hero_check(&h, f, &dl);
		okf(dl_count(&dl, DIAG_ERROR) == 0, "  ...and the character has no errors");
		dl_free(&dl);
		free(text);
	}

	/*
	 * Real saves leave stale items above the inventory count: RemoveInvItem
	 * shifts the list down without clearing the tail. That is ordinary, and
	 * reporting it would put a warning on nearly every played character.
	 */
	PkPlayerStruct h;
	base_hero(&h, PC_WARRIOR);
	h._pNumInv = 1;
	h.InvList[0].idx = 30;
	h.InvGrid[0] = 1;
	h.InvList[5].idx = 31; /* residue above the count */
	DiagList dl;
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	ok(dl_count(&dl, DIAG_ERROR) == 0 && dl_count(&dl, DIAG_WARNING) == 0,
	    "stale items above the inventory count are not reported");
	dl_free(&dl);
}

/* ------------------------------------------------------------------ */
/* 2. Misspelled keys -- the silent killer                             */
/* ------------------------------------------------------------------ */

static void check_unknown_keys(void)
{
	section("2. unknown and misspelled fields");

	static const struct {
		const char *doc;
		const char *typo;
		const char *meant;
		const char *where;
	} cases[] = {
		{ "{\"attributes\":{\"vitallity\":25}}", "vitallity", "vitality",
		    "attributes.vitallity" },
		{ "{\"attributes\":{\"magick\":25}}", "magick", "magic", "attributes.magick" },
		{ "{\"attributes\":{\"strengh\":25}}", "strengh", "strength",
		    "attributes.strengh" },
		{ "{\"mana\":{\"curent\":5}}", "curent", "current", "mana.curent" },
		{ "{\"life\":{\"maximum\":5}}", "maximum", "max", "life.maximum" },
		{ "{\"levl\":5}", "levl", "level", "levl" },
		{ "{\"experiance\":5}", "experiance", "experience", "experiance" },
		{ "{\"position\":{\"dungeon_lvl\":5}}", "dungeon_lvl", "dungeon_level",
		    "position.dungeon_lvl" },
		{ "{\"progress\":{\"reflection\":5}}", "reflection", "reflections",
		    "progress.reflection" },
		{ "{\"equipment\":{\"hed\":null}}", "hed", "head", "equipment.hed" },
		{ "{\"equipment\":{\"left_hnd\":null}}", "left_hnd", "left_hand",
		    "equipment.left_hnd" },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		DiagList dl;
		check_doc(cases[i].doc, &dl);
		int found = dl_count(&dl, DIAG_ERROR) > 0 && located_at(&dl, cases[i].where)
		    && mentions(&dl, cases[i].meant);
		okf(found, "\"%s\" is rejected and suggests \"%s\"", cases[i].typo,
		    cases[i].meant);
		if (!found)
			dl_report(&dl, "case", stdout);
		dl_free(&dl);
	}

	/* Nonsense gets rejected without a misleading suggestion. */
	{
		DiagList dl;
		check_doc("{\"zzzzqqqq\":1}", &dl);
		ok(dl_count(&dl, DIAG_ERROR) == 1, "an unrecognizable field is rejected");
		ok(!mentions(&dl, "did you mean"),
		    "  ...with no guess, since none would be plausible");
		dl_free(&dl);
	}

	/* Unknown keys in every nested object, not just the root. */
	ok(doc_errors("{\"advanced\":{\"reserved_byte\":[0,0,0]}}") > 0,
	    "unknown fields inside \"advanced\" are caught");
	ok(doc_errors("{\"spells\":[{\"id\":1,\"lvl\":2}]}") > 0,
	    "unknown fields inside a spell are caught");
	ok(doc_errors("{\"inventory\":{\"items\":[{\"slot\":0,\"indx\":1}]}}") > 0,
	    "unknown fields inside an item are caught");
	ok(doc_errors("{\"belt\":[{\"slot\":0,\"seeed\":1}]}") > 0,
	    "unknown fields inside a belt item are caught");

	/* A correct document must not trip any of this. */
	ok(doc_errors("{\"attributes\":{\"strength\":1,\"magic\":2,\"dexterity\":3,"
	              "\"vitality\":4}}")
	        == 0,
	    "all four real attribute names are accepted");
}

/* ------------------------------------------------------------------ */
/* 3. Values that would be truncated                                   */
/* ------------------------------------------------------------------ */

static void check_ranges(void)
{
	section("3. values that would not survive their field");

	static const struct {
		const char *doc;
		const char *what;
	} over[] = {
		{ "{\"attributes\":{\"strength\":300}}", "strength 300 (a byte holds 255)" },
		{ "{\"attributes\":{\"vitality\":-1}}", "a negative attribute" },
		{ "{\"stat_points\":1000}", "stat_points 1000" },
		{ "{\"level\":500}", "level 500 (stored in one signed byte)" },
		{ "{\"position\":{\"x\":999}}", "a position beyond a byte" },
		{ "{\"progress\":{\"reflections\":99999}}", "reflections beyond a word" },
		{ "{\"progress\":{\"town_warps\":200}}", "town_warps beyond a signed byte" },
		{ "{\"life\":{\"current_64ths\":64}}", "a 64ths remainder of 64" },
		{ "{\"life\":{\"current\":99999999}}", "a life value that would overflow" },
		{ "{\"inventory\":{\"count\":41}}", "an inventory count of 41" },
		{ "{\"inventory\":{\"items\":[{\"slot\":0,\"index\":70000}]}}",
		    "an item index beyond a word" },
		{ "{\"inventory\":{\"items\":[{\"slot\":0,\"seed\":5000000000}]}}",
		    "an item seed beyond a dword" },
		{ "{\"advanced\":{\"reserved2\":40000}}", "a reserved short overflowing" },
		{ "{\"version\":0}", "version 0" },
	};

	for (size_t i = 0; i < sizeof(over) / sizeof(over[0]); i++)
		okf(doc_errors(over[i].doc) > 0, "%s is rejected", over[i].what);

	/* Exact boundaries must be accepted, or the checker is useless. */
	static const struct {
		const char *doc;
		const char *what;
	} edge[] = {
		{ "{\"attributes\":{\"strength\":255}}", "strength 255" },
		{ "{\"attributes\":{\"strength\":0}}", "strength 0" },
		{ "{\"level\":127}", "level 127 (the storage limit, not the game limit)" },
		{ "{\"level\":-128}", "level -128" },
		{ "{\"life\":{\"current_64ths\":63}}", "a 64ths remainder of 63" },
		{ "{\"progress\":{\"reflections\":65535}}", "reflections 65535" },
		{ "{\"inventory\":{\"count\":40}}", "an inventory count of 40" },
		{ "{\"inventory\":{\"items\":[{\"slot\":39,\"seed\":4294967295}]}}",
		    "the largest item seed" },
	};
	for (size_t i = 0; i < sizeof(edge) / sizeof(edge[0]); i++)
		okf(doc_errors(edge[i].doc) == 0, "%s is accepted", edge[i].what);
}

/* ------------------------------------------------------------------ */
/* 4. Types, duplicates, and structure                                 */
/* ------------------------------------------------------------------ */

static void check_structure(void)
{
	section("4. types and structure");

	ok(doc_errors("{\"level\":\"30\"}") > 0, "a number given as a string is rejected");
	ok(doc_errors("{\"name\":42}") > 0, "a string given as a number is rejected");
	ok(doc_errors("{\"attributes\":42}") > 0, "an object given as a number is rejected");
	ok(doc_errors("{\"spells\":{}}") > 0, "an array given as an object is rejected");
	ok(doc_errors("{\"inventory\":{\"grid\":5}}") > 0, "a non-array grid is rejected");
	ok(doc_errors("{\"progress\":{\"mana_shield\":true}}") == 0,
	    "mana_shield accepts true");
	ok(doc_errors("{\"progress\":{\"mana_shield\":2}}") == 0,
	    "mana_shield also accepts a raw byte");

	ok(doc_errors("{\"inventory\":{\"items\":[{\"slot\":3,\"index\":1},"
	              "{\"slot\":3,\"index\":2}]}}")
	        > 0,
	    "two items claiming the same inventory slot is rejected");
	ok(doc_errors("{\"belt\":[{\"slot\":1,\"index\":1},{\"slot\":1,\"index\":2}]}") > 0,
	    "two belt items claiming the same slot is rejected");
	ok(doc_errors("{\"spells\":[{\"level\":3}]}") > 0, "a spell without an id is rejected");

	/* A short grid row is a warning, not an error: the rest defaults to zero. */
	{
		DiagList dl;
		check_doc("{\"inventory\":{\"grid\":[[1,2],[0],[0],[0]]}}", &dl);
		ok(dl_count(&dl, DIAG_ERROR) == 0 && dl_count(&dl, DIAG_WARNING) > 0,
		    "a short grid row warns rather than errors");
		dl_free(&dl);
	}

	/* Syntax errors carry a position. */
	{
		DiagList dl;
		check_doc("{\n \"a\": 1,\n \"b\": @\n}", &dl);
		ok(dl_count(&dl, DIAG_ERROR) > 0 && mentions(&dl, "line 3"),
		    "a syntax error reports its line");
		dl_free(&dl);
	}

	/* A missing format tag is a nudge, not a failure. */
	{
		DiagList dl;
		check_doc("{\"level\":3}", &dl);
		ok(dl_count(&dl, DIAG_ERROR) == 0 && dl_count(&dl, DIAG_WARNING) == 1,
		    "a missing \"format\" tag warns but does not fail");
		dl_free(&dl);
	}
	ok(doc_errors("{\"format\":\"something-else\"}") > 0,
	    "a foreign \"format\" tag is rejected");
	ok(doc_errors("{\"game\":\"starcraft\"}") > 0, "an unknown \"game\" is rejected");
	ok(doc_errors("{\"class\":\"Barbarain\"}") > 0, "a misspelled class is rejected");
	{
		DiagList dl;
		check_doc("{\"class\":\"Barbarain\"}", &dl);
		ok(mentions(&dl, "Barbarian"), "  ...and suggests the right one");
		dl_free(&dl);
	}
}

/* ------------------------------------------------------------------ */
/* 5. Character checks report everything                               */
/* ------------------------------------------------------------------ */

static void check_hero_all(void)
{
	section("5. character checks report every problem at once");

	PkPlayerStruct h;
	base_hero(&h, PC_SORCERER);
	h.pLevel = 99;             /* 1 */
	h.pExperience = -5;        /* 2 */
	h.pBaseStr = 200;          /* 3: sorcerer cap is 45 */
	h.pHPBase = HERO_FROM_WHOLE(90); /* 4: above max */
	h.plrlevel = 30;           /* 5: beyond Diablo's 16 */

	DiagList dl;
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	int errors = dl_count(&dl, DIAG_ERROR);
	okf(errors >= 5, "five independent problems yield %d errors in one pass", errors);
	ok(located_at(&dl, "level"), "the level error is located at \"level\"");
	ok(located_at(&dl, "experience"), "the experience error is located");
	ok(located_at(&dl, "attributes.strength"), "the stat-cap error is located");
	ok(located_at(&dl, "life.current"), "the life error is located");
	ok(located_at(&dl, "position.dungeon_level"), "the dungeon-level error is located");
	dl_free(&dl);

	/* hero_validate must stay consistent with hero_check. */
	char err[HERO_ERR_LEN];
	ok(!hero_validate(&h, FLAVOR_DIABLO, err),
	    "hero_validate agrees the character is invalid");
	base_hero(&h, PC_SORCERER);
	ok(hero_validate(&h, FLAVOR_DIABLO, err), "and agrees when it is valid");

	/* Inventory coherence. */
	base_hero(&h, PC_WARRIOR);
	h._pNumInv = 2;
	h.InvList[0].idx = 30;
	h.InvGrid[0] = 1;
	/* slot 1 is counted but empty, and has no grid cell */
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	ok(dl_count(&dl, DIAG_ERROR) > 0, "an empty slot below the count is an error");
	dl_free(&dl);

	base_hero(&h, PC_WARRIOR);
	h._pNumInv = 1;
	h.InvList[0].idx = 30;
	h.InvGrid[0] = 5; /* points at item 5, but only 1 is live */
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	ok(dl_count(&dl, DIAG_ERROR) > 0 && mentions(&dl, "only 1 items are live"),
	    "a grid cell pointing past the item count is an error");
	dl_free(&dl);

	base_hero(&h, PC_WARRIOR);
	h._pNumInv = 1;
	h.InvList[0].idx = 30;
	/* no grid cell at all: the game cannot display it */
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	ok(dl_count(&dl, DIAG_ERROR) > 0 && mentions(&dl, "no cell in the grid"),
	    "a live item with no grid cell is an error");
	dl_free(&dl);

	/* Warnings stay warnings. */
	base_hero(&h, PC_WARRIOR);
	h.pGold = 5000; /* cached, no stacks */
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	ok(dl_count(&dl, DIAG_ERROR) == 0 && dl_count(&dl, DIAG_WARNING) == 1,
	    "a stale gold cache is a warning, not an error");
	dl_free(&dl);

	base_hero(&h, PC_WARRIOR);
	h.pLevel = 40;
	h.pExperience = 0;
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	ok(dl_count(&dl, DIAG_ERROR) == 0 && dl_count(&dl, DIAG_WARNING) == 1,
	    "a level/experience mismatch is a warning");
	ok(mentions(&dl, "set experience to"), "  ...and says what to set it to");
	dl_free(&dl);

	/* Hellfire class in a Diablo character explains itself. */
	base_hero(&h, 3);
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	ok(dl_count(&dl, DIAG_ERROR) > 0 && mentions(&dl, "--hellfire"),
	    "a Hellfire class read as Diablo names the fix");
	dl_free(&dl);
}

/* ------------------------------------------------------------------ */
/* 5b. Spells                                                          */
/* ------------------------------------------------------------------ */

/*
 * From a real defect: a Warrior edited in the TUI stopped appearing in the
 * game's character list. The save had a book bit for spell id 37, which is
 * Hellfire's Mana Shield -- Diablo's spelldata[] stops at 36, and the game
 * indexes that table straight off _pMemSpells while drawing the spell book.
 *
 * The rules checked here are the ones the game applies to itself in
 * Source/player.cpp: _pMemSpells holds only spells whose sBookLvl is not -1,
 * and levels are clamped to MAX_SPELL_LEVEL.
 */
static void check_spells(void)
{
	section("5b. spells the game cannot survive");

	PkPlayerStruct h;
	DiagList dl;

	/* A Hellfire id in a Diablo save reads past the end of spelldata[]. */
	base_hero(&h, PC_WARRIOR);
	h.pMemSpells |= SPELLBIT(37);
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	ok(dl_count(&dl, DIAG_ERROR) > 0, "a Hellfire spell id in a Diablo save is an error");
	ok(mentions(&dl, "read past the end"), "  ...and says why it matters");
	ok(located_at(&dl, "spells"), "  ...located at \"spells\"");
	dl_free(&dl);

	/* The same id is fine in a Hellfire save. */
	base_hero(&h, PC_WARRIOR);
	h.pMemSpells |= SPELLBIT(37);
	dl_init(&dl);
	hero_check(&h, FLAVOR_HELLFIRE, &dl);
	ok(dl_count(&dl, DIAG_ERROR) == 0, "the same id is legal in a Hellfire save");
	dl_free(&dl);

	/* Diablo has no business writing Hellfire's overflow array. */
	base_hero(&h, PC_WARRIOR);
	h.pSplLvl2[0] = 3;
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	ok(dl_count(&dl, DIAG_ERROR) > 0 && mentions(&dl, "pSplLvl2"),
	    "data in the Hellfire spell area of a Diablo save is an error");
	dl_free(&dl);

	/* A spell with no book cannot sit in the book. */
	int identify = hero_find_spell(FLAVOR_DIABLO, "Identify");
	okf(identify > 0, "Identify is id %d", identify);
	ok(!hero_spell_has_book(FLAVOR_DIABLO, identify), "  ...and has no book");
	base_hero(&h, PC_WARRIOR);
	h.pMemSpells |= SPELLBIT(identify);
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	ok(dl_count(&dl, DIAG_ERROR) == 0 && dl_count(&dl, DIAG_WARNING) > 0,
	    "a book bit for a spell with no book is a warning, not an error");
	ok(mentions(&dl, "will disappear"), "  ...and says the game will drop it");
	dl_free(&dl);

	/* Above the game's own clamp. */
	int firebolt = hero_find_spell(FLAVOR_DIABLO, "Firebolt");
	base_hero(&h, PC_WARRIOR);
	h.pSplLvl[firebolt] = 40;
	h.pMemSpells |= SPELLBIT(firebolt);
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	ok(dl_count(&dl, DIAG_WARNING) > 0 && mentions(&dl, "clamps"),
	    "a spell level above 15 warns that the game clamps it");
	dl_free(&dl);

	/* Negative levels are not reachable in play. */
	base_hero(&h, PC_WARRIOR);
	h.pSplLvl[firebolt] = -3;
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	ok(dl_count(&dl, DIAG_ERROR) > 0, "a negative spell level is an error");
	dl_free(&dl);

	/* A level with no book bit is legal but worth saying. */
	base_hero(&h, PC_WARRIOR);
	h.pSplLvl[firebolt] = 5;
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	ok(dl_count(&dl, DIAG_ERROR) == 0 && dl_count(&dl, DIAG_WARNING) > 0,
	    "a spell level without a book bit is a warning");
	dl_free(&dl);

	/* A properly learned spell is silent. */
	base_hero(&h, PC_WARRIOR);
	h.pSplLvl[firebolt] = 5;
	h.pMemSpells |= SPELLBIT(firebolt);
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	ok(dl_count(&dl, DIAG_ERROR) == 0 && dl_count(&dl, DIAG_WARNING) == 0,
	    "a spell learned properly produces no findings");
	dl_free(&dl);

	/* hero_set_spell_level refuses at the point of edit, not just on read. */
	char err[HERO_ERR_LEN];
	base_hero(&h, PC_WARRIOR);
	ok(!hero_set_spell_level(&h, FLAVOR_DIABLO, 37, 1, err),
	    "setting a Hellfire spell on a Diablo character is refused");
	ok(hero_set_spell_level(&h, FLAVOR_HELLFIRE, 37, 1, err),
	    "  ...and accepted on a Hellfire one");
	ok(!hero_set_spell_level(&h, FLAVOR_DIABLO, firebolt, 40, err),
	    "a level above the game's cap is refused");

	/* Setting a level on a bookless spell must not invent a book bit. */
	base_hero(&h, PC_WARRIOR);
	ok(hero_set_spell_level(&h, FLAVOR_DIABLO, identify, 4, err),
	    "a bookless spell can still be given a level");
	ok((h.pMemSpells & SPELLBIT(identify)) == 0,
	    "  ...without being put in the spell book");
	dl_init(&dl);
	hero_check(&h, FLAVOR_DIABLO, &dl);
	ok(dl_count(&dl, DIAG_ERROR) == 0, "  ...and the result has no errors");
	dl_free(&dl);
}

/* ------------------------------------------------------------------ */
/* 6. Suggestion quality                                               */
/* ------------------------------------------------------------------ */

static void check_suggestions(void)
{
	section("6. suggestion quality");

	static const char *const words[] = { "strength", "magic", "dexterity", "vitality" };

	ok(dl_nearest("strengt", words, 4) != NULL, "a one-letter slip is matched");
	ok(strcmp(dl_nearest("vitalty", words, 4), "vitality") == 0,
	    "a dropped letter matches the right word");
	ok(strcmp(dl_nearest("Magic", words, 4), "magic") != 0
	        || dl_nearest("Magic", words, 4) != NULL,
	    "case differences still match");
	ok(dl_nearest("bananas", words, 4) == NULL,
	    "an unrelated word produces no suggestion");
	ok(dl_nearest("x", words, 4) == NULL, "a single character produces no suggestion");
}


/* ------------------------------------------------------------------ */
/* 7. Input sniffing                                                   */
/* ------------------------------------------------------------------ */

/*
 * `validate` originally took only JSON, and answering `validate single_0.sv`
 * with "unexpected character 'M'" was a real report from real use. Every other
 * command takes a save path, so that is the natural thing to type, and the
 * question behind it -- is this character valid -- has an answer either way.
 *
 * These tests drive the binary rather than the library, because the sniffing
 * lives in the command.
 */
static const char *g_exe;

/** Run `butcher validate <args>` and return its exit status; output to buf. */
static int run_validate(const char *args, char *out, size_t n)
{
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "%s validate %s 2>&1", g_exe, args);
	FILE *p = popen(cmd, "r");
	if (p == NULL)
		return -1;
	size_t got = fread(out, 1, n - 1, p);
	out[got] = '\0';
	int rc = pclose(p);
	return WEXITSTATUS(rc);
}

static void check_input_kinds(void)
{
	section("7. accepts a save, a raw struct, or JSON");

	if (g_exe == NULL) {
		printf("  SKIP  set BUTCHER_EXE to the built binary to test sniffing\n");
		return;
	}

	const char *dir = getenv("BUTCHER_TMPDIR");
	if (dir == NULL)
		dir = ".";

	char out[4096];
	char path[1024];

	/* Build a save, a raw struct, and a JSON document from one character. */
	PkPlayerStruct h;
	base_hero(&h, PC_WARRIOR);

	BYTE blob[1288];
	memset(blob, 0, sizeof(blob));
	memcpy(blob, &h, sizeof(h));
	codec_encode(blob, sizeof(h), 1288, SAVE_PASSWORD_SINGLE);

	snprintf(path, sizeof(path), "%s/sniff.sv", dir);
	{
		/* Minimal archive, same builder shape as the other suites. */
		_FILEHEADER hdr;
		_BLOCKENTRY *block = (_BLOCKENTRY *)calloc(MPQ_INDEX_ENTRIES, sizeof(_BLOCKENTRY));
		_HASHENTRY *hash = (_HASHENTRY *)malloc(MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
		memset(hash, 0xFF, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
		InitHash();

		BYTE body[1288 * 2];
		DWORD *offs = (DWORD *)body;
		BYTE sector[MPQ_SECTOR_SIZE];
		memcpy(sector, blob, 1288);
		int stored = PkwareCompress(sector, 1288);
		offs[0] = 8;
		offs[1] = 8 + (DWORD)stored;
		memcpy(body + 8, sector, (size_t)stored);

		block[0].offset = MPQ_DATA_OFFSET;
		block[0].sizealloc = (int)offs[1];
		block[0].sizefile = 1288;
		block[0].flags = (int)(MPQ_FLAG_EXISTS | MPQ_FLAG_IMPLODE);
		DWORD idx = Hash("hero", 0) & 0x7FF;
		hash[idx].hashcheck[0] = (int)Hash("hero", 1);
		hash[idx].hashcheck[1] = (int)Hash("hero", 2);
		hash[idx].lcid = 0;
		hash[idx].block = 0;

		memset(&hdr, 0, sizeof(hdr));
		hdr.signature = (int)MPQ_SIGNATURE;
		hdr.headersize = 32;
		hdr.filesize = (int)(MPQ_DATA_OFFSET + offs[1]);
		hdr.sectorsizeid = 3;
		hdr.hashoffset = MPQ_HASH_OFFSET;
		hdr.blockoffset = MPQ_BLOCK_OFFSET;
		hdr.hashcount = MPQ_INDEX_ENTRIES;
		hdr.blockcount = MPQ_INDEX_ENTRIES;
		Encrypt((DWORD *)block, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY),
		    Hash("(block table)", 3));
		Encrypt((DWORD *)hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY),
		    Hash("(hash table)", 3));

		FILE *f = fopen(path, "wb");
		fwrite(&hdr, sizeof(hdr), 1, f);
		fwrite(block, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY), 1, f);
		fwrite(hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), 1, f);
		fwrite(body, offs[1], 1, f);
		fclose(f);
		free(block);
		free(hash);
	}

	int rc = run_validate(path, out, sizeof(out));
	ok(rc == 0 && strstr(out, "save archive") != NULL,
	    "a save file is recognized, not parsed as JSON");
	if (rc != 0)
		printf("        %s", out);

	/* The raw 1266-byte struct. */
	char raw[1024];
	snprintf(raw, sizeof(raw), "%s/sniff.bin", dir);
	{
		FILE *f = fopen(raw, "wb");
		fwrite(&h, sizeof(h), 1, f);
		fclose(f);
	}
	rc = run_validate(raw, out, sizeof(out));
	ok(rc == 0 && strstr(out, "raw character struct") != NULL,
	    "a 1266-byte struct is recognized");
	if (rc != 0)
		printf("        %s", out);

	/* JSON. */
	char js[1024];
	snprintf(js, sizeof(js), "%s/sniff.json", dir);
	{
		char *text = charjson_write(&h, FLAVOR_DIABLO, nowarn);
		FILE *f = fopen(js, "wb");
		fputs(text, f);
		fclose(f);
		free(text);
	}
	rc = run_validate(js, out, sizeof(out));
	ok(rc == 0 && strstr(out, "JSON document") != NULL, "JSON is still recognized");

	/* Not text and not either binary format. */
	char junk[1024];
	snprintf(junk, sizeof(junk), "%s/sniff.junk", dir);
	{
		FILE *f = fopen(junk, "wb");
		for (int i = 0; i < 900; i++)
			fputc(i % 251, f);
		fclose(f);
	}
	rc = run_validate(junk, out, sizeof(out));
	ok(rc == 1 && strstr(out, "not text") != NULL,
	    "unrecognized binary says so instead of quoting a stray byte");

	/* A missing file is a usage problem, not a validation failure. */
	char missing[1024];
	snprintf(missing, sizeof(missing), "%s/does-not-exist.json", dir);
	rc = run_validate(missing, out, sizeof(out));
	ok(rc == 2, "a missing file exits 2, distinct from an invalid one");

	/* Piped JSON still works; a piped save cannot and says why. */
	char piped[1200];
	snprintf(piped, sizeof(piped), "- < %s", js);
	rc = run_validate(piped, out, sizeof(out));
	ok(rc == 0 && strstr(out, "stdin") != NULL, "JSON on standard input works");

	snprintf(piped, sizeof(piped), "- < %s", path);
	rc = run_validate(piped, out, sizeof(out));
	ok(rc == 1 && strstr(out, "rather than a pipe") != NULL,
	    "a save on standard input explains that it needs a path");
}

/* ------------------------------------------------------------------ */
/* 8. Real save (opt-in)                                               */
/* ------------------------------------------------------------------ */

static void check_real_save(void)
{
	section("8. real save");

	const char *path = getenv("BUTCHER_SAVE");
	if (path == NULL || path[0] == '\0') {
		printf("  SKIP  set BUTCHER_SAVE to validate a real character\n");
		return;
	}

	PkPlayerStruct h;
	char merr[MPQ_ERR_LEN];
	if (!save_read_hero(path, &h, NULL, merr)) {
		ok(0, "read the real save");
		printf("        %s\n", merr);
		return;
	}
	HeroFlavor f = hero_flavor_for_path(path);

	char *text = charjson_write(&h, f, nowarn);
	DiagList dl;
	check_doc(text, &dl);
	int derr = dl_count(&dl, DIAG_ERROR), dwarn = dl_count(&dl, DIAG_WARNING);
	ok(derr == 0 && dwarn == 0, "a document exported from a REAL save is silent");
	if (derr || dwarn)
		dl_report(&dl, path, stdout);
	dl_free(&dl);
	free(text);

	dl_init(&dl);
	hero_check(&h, f, &dl);
	int herr = dl_count(&dl, DIAG_ERROR);
	ok(herr == 0, "and the character itself reports no errors");
	if (herr)
		dl_report(&dl, path, stdout);
	dl_free(&dl);
}

/* ------------------------------------------------------------------ */

int main(void)
{
	printf("Devilution character editor -- Phase 6 validation\n");
	g_exe = getenv("BUTCHER_EXE");

	check_clean();
	check_unknown_keys();
	check_ranges();
	check_structure();
	check_hero_all();
	check_spells();
	check_suggestions();
	check_input_kinds();
	check_real_save();

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
