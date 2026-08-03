/**
 * @file editor_model.cpp
 *
 * Gate for the TUI's editor model.
 *
 * These exist because of a real crash: moving to the Spells pane segfaulted.
 * The cause was that FTXUI widgets bind to a raw address, and the spell arrays
 * were std::vectors that Load() reassigned -- so every spell slider pointed at
 * memory that had moved. Worse, when the picker was shown first the vectors
 * were still empty when the widgets were built, so they bound to &v[1] on an
 * empty vector, which is address 0x4.
 *
 * Neither is visible from reading a render; both are trivially testable here.
 */
#include "../tui/editor.h"

#include "../third_party/devilution/Source/encrypt.h"

#include <stdarg.h>
#include <sys/stat.h>

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

static void section(const char *name)
{
	printf("\n%s\n", name);
}

/* ------------------------------------------------------------------ */

static void base_hero(PkPlayerStruct *p, const char *name, int pclass)
{
	memset(p, 0, sizeof(*p));
	strncpy(p->pName, name, PLR_NAME_LEN - 1);
	p->pClass = (char)pclass;
	p->pLevel = 10;
	p->pExperience = hero_exp_for_level(10);
	p->pBaseStr = 30;
	p->pBaseMag = 20;
	p->pBaseDex = 30;
	p->pBaseVit = 30;
	p->pHPBase = HERO_FROM_WHOLE(50);
	p->pMaxHPBase = HERO_FROM_WHOLE(50);
	p->pManaBase = HERO_FROM_WHOLE(25);
	p->pMaxManaBase = HERO_FROM_WHOLE(25);
	p->plrlevel = 3;
	for (int i = 0; i < NUM_INVLOC; i++)
		p->InvBody[i].idx = 0xFFFF;
	for (int i = 0; i < NUM_INV_GRID_ELEM; i++)
		p->InvList[i].idx = 0xFFFF;
	for (int i = 0; i < MAXBELTITEMS; i++)
		p->SpdList[i].idx = 0xFFFF;
}

static SaveEntry make_entry(const char *path, const PkPlayerStruct &h)
{
	SaveEntry e {};
	snprintf(e.path, sizeof(e.path), "%s", path);
	memcpy(e.name, h.pName, PLR_NAME_LEN);
	e.name[PLR_NAME_LEN] = '\0';
	e.slot = 0;
	e.flavor = hero_flavor_for_path(path);
	e.hero = h;
	return e;
}

/* ------------------------------------------------------------------ */
/* 1. Address stability -- the crash                                   */
/* ------------------------------------------------------------------ */

static void check_addresses(void)
{
	section("1. every bindable field keeps its address");

	Editor ed;
	PkPlayerStruct a, b;
	base_hero(&a, "Aidan", PC_WARRIOR);
	base_hero(&b, "Jazreth", 3); /* a Hellfire Monk */

	/*
	 * A freshly constructed Editor must already be safe to bind to. The
	 * picker path built widgets before any character was loaded, and
	 * &vector[1] on an empty vector is 0x4.
	 */
	const void *fresh_spell = &ed.spell_lvl[1];
	const void *fresh_known = &ed.spell_known[1];
	ok(fresh_spell != nullptr && (uintptr_t)fresh_spell > 0x1000,
	    "spell levels are addressable before any character is loaded");
	ok(fresh_known != nullptr && (uintptr_t)fresh_known > 0x1000,
	    "spell-book flags likewise");

	/* Record every address a widget binds to. */
	const void *addr[] = {
		&ed.name, &ed.cls, &ed.str, &ed.mag, &ed.dex, &ed.vit, &ed.statpts,
		&ed.level_target, &ed.hp, &ed.hp_max, &ed.mana, &ed.mana_max,
		&ed.gold, &ed.dlvl, &ed.cap_str, &ed.cap_mag, &ed.cap_dex,
		&ed.cap_vit, &ed.cap_dlvl, &ed.cap_gold,
		&ed.spell_lvl[1], &ed.spell_lvl[46],
		&ed.spell_known[1], &ed.spell_known[46],
	};
	const int n = (int)(sizeof(addr) / sizeof(addr[0]));
	const void *before[n];
	memcpy(before, addr, sizeof(addr));

	/* Load a Diablo character, then a Hellfire one, then revert. Each is a
	 * path the UI takes with widgets already bound. */
	ed.Load(make_entry("/tmp/single_0.sv", a));
	ed.Load(make_entry("/tmp/single_0.hsv", b));
	ed.Load(make_entry("/tmp/single_0.sv", a));

	const void *after[] = {
		&ed.name, &ed.cls, &ed.str, &ed.mag, &ed.dex, &ed.vit, &ed.statpts,
		&ed.level_target, &ed.hp, &ed.hp_max, &ed.mana, &ed.mana_max,
		&ed.gold, &ed.dlvl, &ed.cap_str, &ed.cap_mag, &ed.cap_dex,
		&ed.cap_vit, &ed.cap_dlvl, &ed.cap_gold,
		&ed.spell_lvl[1], &ed.spell_lvl[46],
		&ed.spell_known[1], &ed.spell_known[46],
	};
	int moved = 0;
	for (int i = 0; i < n; i++)
		if (before[i] != after[i])
			moved++;
	ok(moved == 0, "no bound field moves across three loads");

	/* And the arrays are big enough for every id a save can carry. */
	ok(Editor::kSpellSlots >= hero_spell_persisted(),
	    "the spell arrays cover every persistable spell id");
}

/* ------------------------------------------------------------------ */
/* 2. Load and Compose                                                 */
/* ------------------------------------------------------------------ */

static void check_roundtrip(void)
{
	section("2. loading and composing");

	Editor ed;
	PkPlayerStruct h;
	base_hero(&h, "Aidan", PC_WARRIOR);
	h.pSplLvl[3] = 5;
	h.pMemSpells |= SPELLBIT(3);
	h.pHPBase = 3458; /* carries a fractional remainder */
	h.pMaxHPBase = 3458;

	ed.Load(make_entry("/tmp/single_0.sv", h));

	/* Opening a character and touching nothing must change nothing. */
	PkPlayerStruct out = ed.Compose();
	ok(memcmp(&out, &h, sizeof(h)) == 0, "load then compose is byte-identical");
	ok(!ed.Dirty(), "and the editor does not consider itself modified");
	ok(out.pHPBase == 3458, "the fractional life remainder survives");

	/* Each edit reaches the struct. */
	ed.str = 44;
	ok(ed.Compose().pBaseStr == 44, "strength");
	ed.name = "Griswold";
	ok(strcmp(ed.Compose().pName, "Griswold") == 0, "name");
	ed.spell_lvl[3] = 9;
	PkPlayerStruct with_spell = ed.Compose();
	ok(hero_get_spell_level(&with_spell, 3) == 9, "a spell level");
	ed.spell_known[3] = false;
	ok((ed.Compose().pMemSpells & SPELLBIT(3)) == 0, "clearing a spell-book bit");
	ok(ed.Dirty(), "the editor now reports itself modified");

	/*
	 * The level slider levels the character up. Writing experience alone and
	 * leaving the level for the game to award does not survive: ValidatePlayer
	 * clamps experience back down to _pNextExper on the first tick.
	 */
	ed.Load(make_entry("/tmp/single_0.sv", h));
	ok(ed.Compose().pExperience == h.pExperience,
	    "an untouched level slider leaves experience exactly as it was");
	ed.level_target = 20;
	ed.ApplyLevelTarget();
	PkPlayerStruct raised = ed.Compose();
	ok(raised.pLevel == 20, "raising it writes the level");
	ok(raised.pExperience == hero_exp_for_level(20), "and the experience to match");
	ok(raised.pExperience <= hero_next_exper(&raised),
	    "  ...which is what keeps ValidatePlayer from clamping it away");

	/* Gold rewrites the inventory, so it must not run unless gold changed. */
	ed.Load(make_entry("/tmp/single_0.sv", h));
	PkPlayerStruct untouched_gold = ed.Compose();
	ok(memcmp(&untouched_gold, &h, sizeof(h)) == 0,
	    "an untouched gold slider leaves the inventory alone");
	ed.gold = 12000;
	PkPlayerStruct rich = ed.Compose();
	ok(rich.pGold == 12000 && hero_gold_in_stacks(&rich) == 12000,
	    "changing it distributes real stacks");
}

/* ------------------------------------------------------------------ */
/* 3. Caps follow the class                                            */
/* ------------------------------------------------------------------ */

static void check_caps(void)
{
	section("3. caps follow the class");

	Editor ed;
	PkPlayerStruct h;
	base_hero(&h, "Jazreth", 3); /* Monk */
	ed.Load(make_entry("/tmp/single_0.hsv", h));

	ok(ed.cap_str == 150 && ed.cap_mag == 80, "a Monk's caps are loaded");

	/* Switching class must move the ceilings the sliders are bound to. */
	ed.cls = 5; /* Barbarian */
	ed.RefreshCaps();
	ok(ed.cap_str == 255 && ed.cap_mag == 0, "switching to Barbarian moves them");

	/*
	 * A lower ceiling must pull the value down with it, or the sheet would
	 * show a number the game rejects.
	 */
	ed.cls = 3;
	ed.RefreshCaps();
	ed.mag = 80;
	ed.cls = 5; /* Barbarian: magic cap 0 */
	ed.RefreshCaps();
	ok(ed.mag == 0, "a value above a newly lowered cap is clamped");

	char err[HERO_ERR_LEN];
	PkPlayerStruct clamped = ed.Compose();
	ok(hero_validate(&clamped, FLAVOR_HELLFIRE, err),
	    "so the composed character still validates");

	/* Diablo has no Monk, and its dungeon is shorter. */
	ed.Load(make_entry("/tmp/single_0.sv", h));
	ok(ed.cap_dlvl == 16, "a Diablo save caps the dungeon at 16");
	ed.Load(make_entry("/tmp/single_0.hsv", h));
	ok(ed.cap_dlvl == 24, "a Hellfire save at 24");
}

/* ------------------------------------------------------------------ */
/* 3b. Spells stay inside the save's own game                          */
/* ------------------------------------------------------------------ */

/*
 * From a real defect: a Warrior edited here stopped appearing in the game's
 * character list. The spell pane is built once and covers both games, hiding
 * rows the current flavour lacks at render time -- but hidden is not the same
 * as absent, and Compose() wrote them all. The save ended up with a book bit
 * for id 37, which Diablo's 37-row spelldata[] cannot index.
 */
static void check_spell_flavor(void)
{
	section("3b. composing cannot write a spell the save's game lacks");

	Editor ed;
	PkPlayerStruct h;
	base_hero(&h, "Aidan", PC_WARRIOR);

	ed.Load(make_entry("/tmp/single_0.sv", h));

	/* Whatever the pane leaves set, a Diablo save must not receive it. */
	for (int s = 0; s < Editor::kSpellSlots; s++) {
		ed.spell_lvl[s] = 3;
		ed.spell_known[s] = true;
	}
	PkPlayerStruct out = ed.Compose();

	int stray = 0;
	for (int s = hero_spell_slots(FLAVOR_DIABLO); s < hero_spell_persisted(); s++)
		if (hero_get_spell_level(&out, s) != 0 || (out.pMemSpells & SPELLBIT(s)) != 0)
			stray++;
	ok(stray == 0, "no spell id beyond Diablo's table survives Compose");

	int nobook = 0;
	for (int s = 1; s < hero_spell_slots(FLAVOR_DIABLO); s++)
		if ((out.pMemSpells & SPELLBIT(s)) != 0
		    && !hero_spell_has_book(FLAVOR_DIABLO, s))
			nobook++;
	ok(nobook == 0, "and nothing without a book lands in the spell book");

	char err[HERO_ERR_LEN];
	ok(hero_validate(&out, FLAVOR_DIABLO, err), "so the result validates");
	if (!hero_validate(&out, FLAVOR_DIABLO, err))
		printf("        %s\n", err);

	/* Hellfire keeps the ids Diablo cannot hold. */
	ed.Load(make_entry("/tmp/single_0.hsv", h));
	for (int s = 0; s < Editor::kSpellSlots; s++) {
		ed.spell_lvl[s] = 3;
		ed.spell_known[s] = true;
	}
	PkPlayerStruct hf = ed.Compose();
	ok(hero_get_spell_level(&hf, 37) == 3, "a Hellfire save still gets id 37");
	ok(hero_validate(&hf, FLAVOR_HELLFIRE, err), "and validates as Hellfire");

	/*
	 * The repair path: opening a save that already holds a foreign id and
	 * saving it must clear the id rather than preserve it.
	 */
	base_hero(&h, "Aidan", PC_WARRIOR);
	h.pMemSpells |= SPELLBIT(37);
	ed.Load(make_entry("/tmp/single_0.sv", h));
	PkPlayerStruct fixed = ed.Compose();
	ok((fixed.pMemSpells & SPELLBIT(37)) == 0,
	    "opening a damaged save and composing clears the foreign id");
	ok(hero_validate(&fixed, FLAVOR_DIABLO, err), "so a damaged save can be repaired");
}

/* ------------------------------------------------------------------ */
/* 3c. Repair from inside the sheet                                    */
/* ------------------------------------------------------------------ */

/*
 * The sheet refuses to save an invalid character, so ^F is the only way out of
 * one. It must rebase Compose() on the repaired struct -- Compose() builds on
 * `original`, so a fix to something the sheet does not own (the inventory
 * grid, the cached gold total) would be dropped on the next keystroke.
 */
static void check_repair(void)
{
	section("3c. repairing without losing the ability to undo");

	Editor ed;
	PkPlayerStruct h;

	/*
	 * A stale gold cache, and nothing else. Compose() does not write pGold
	 * unless the slider moved, so this isolates the property under test: a
	 * repair to a field the sheet does not own has to survive Compose.
	 */
	base_hero(&h, "Aidan", PC_WARRIOR);
	h.pGold = 5000; /* cached, with no stacks behind it */

	SaveEntry entry = make_entry("/tmp/single_0.sv", h);
	ed.Load(entry);
	ok(!ed.Dirty(), "a freshly opened character is unchanged");

	PkPlayerStruct fixed = ed.Compose();
	DiagList log;
	dl_init(&log);
	int n = hero_fix(&fixed, ed.entry.flavor, /*settle_warnings=*/1, &log);
	dl_free(&log);
	ok(n > 0, "hero_fix finds something to repair");

	ed.ApplyRepair(fixed);

	char err[HERO_ERR_LEN];
	PkPlayerStruct after = ed.Compose();
	ok(hero_validate(&after, FLAVOR_DIABLO, err), "the sheet still composes a valid character");

	/*
	 * If the repair had not been folded into `original`, Compose would rebuild
	 * from the file's struct and this would still read 5000.
	 */
	ok(after.pGold == hero_gold_in_stacks(&after),
	    "a repair to a field the sheet does not own survives Compose");

	ok(ed.Dirty(), "and the character reads as modified, so it can be saved");

	/* Revert means "what is on disk", not "what the repair produced". */
	ed.Load(ed.entry);
	PkPlayerStruct reverted = ed.Compose();
	ok(memcmp(&reverted, &h, sizeof(h)) == 0, "reverting comes back to the file");
	ok(!ed.Dirty(), "  ...and reads as unchanged again");

	/*
	 * Separately: opening a character the sheet cannot reproduce reads as
	 * modified straight away, because Compose sanitises the foreign spell id
	 * it will not write.
	 */
	base_hero(&h, "Aidan", PC_WARRIOR);
	h.pMemSpells |= SPELLBIT(37); /* a Hellfire id in a Diablo save */
	ed.Load(make_entry("/tmp/single_0.sv", h));
	ok(ed.Dirty(), "a damaged character reads as modified the moment it is opened");
	ok((ed.Compose().pMemSpells & SPELLBIT(37)) == 0,
	    "  ...because the sheet refuses to write the foreign id back");
}

/* ------------------------------------------------------------------ */
/* 3d. Levelling has to survive ValidatePlayer                         */
/* ------------------------------------------------------------------ */

/*
 * From three rounds of bug reports: raising a Monk's experience did nothing in
 * game, and one kill then awarded a single level and five stat points.
 *
 * The original design wrote experience and left the level for the game to
 * award. That cannot work. ValidatePlayer runs from ProcessPlayers on every
 * tick and does:
 *
 *     if (_pExperience > _pNextExper) _pExperience = _pNextExper;
 *
 * and _pNextExper is ExpLvlsTbl[_pLevel] -- set by InitPlayer from the stored
 * level when unpacking, and carried in the saved game otherwise. So experience
 * worth more than one level above the stored level is discarded on the first
 * frame, before it can ever be turned into levels.
 *
 * The character must therefore be levelled outright, applying per level what
 * NextPlrLevel grants.
 */
static void check_level_up(void)
{
	section("3d. levelling survives the game's own validation");

	PkPlayerStruct h;
	char err[HERO_ERR_LEN];

	base_hero(&h, "Jazreth", 3); /* Monk */
	h.pLevel = 3;
	h.pExperience = hero_exp_for_level(3);
	h.pStatPts = 0;
	int hp0 = h.pMaxHPBase, mana0 = h.pMaxManaBase;

	int gained = hero_level_up(&h, FLAVOR_HELLFIRE, 20, err);
	ok(gained == 17, "raising a level-3 character to 20 gains 17 levels");
	ok(h.pLevel == 20, "the level is written, not left for the game");
	ok(h.pExperience == hero_exp_for_level(20), "with the experience to match");

	/* The invariant that made all of this fail. */
	ok(h.pExperience <= hero_next_exper(&h),
	    "experience is at or below _pNextExper, so ValidatePlayer leaves it alone");

	/* NextPlrLevel's rewards, per level. */
	ok(h.pStatPts == 17 * 5, "five stat points per level");
	ok(h.pMaxHPBase == hp0 + 17 * 129, "life rises 129 per level in single player");
	ok(h.pHPBase == h.pMaxHPBase, "and is refilled");
	ok(h.pMaxManaBase == mana0 + 17 * 129, "mana likewise for a Monk");
	ok(h.pManaBase == h.pMaxManaBase, "and is refilled");
	ok(hero_validate(&h, FLAVOR_HELLFIRE, err), "the result validates");

	/* A Sorcerer gains half the life; a Barbarian gains no mana. */
	PkPlayerStruct s;
	base_hero(&s, "Jazreth", PC_SORCERER);
	s.pLevel = 1;
	int shp = s.pMaxHPBase;
	hero_level_up(&s, FLAVOR_DIABLO, 2, err);
	ok(s.pMaxHPBase == shp + 65, "a Sorcerer gains 65 life, not 129");

	/*
	 * A Barbarian's mana bump is 0, but NextPlrLevel then adds one for single
	 * player -- so it is 1 per level, not none. Worth pinning: the obvious
	 * reading of the table is wrong.
	 */
	PkPlayerStruct b;
	base_hero(&b, "Grognak", 5); /* Barbarian */
	b.pLevel = 1;
	int bmana = b.pMaxManaBase;
	hero_level_up(&b, FLAVOR_HELLFIRE, 5, err);
	ok(b.pMaxManaBase == bmana + 4, "a Barbarian gains only the single-player +1");

	/* Stat points stop at what the class can still absorb. */
	PkPlayerStruct full;
	base_hero(&full, "Aidan", PC_WARRIOR);
	full.pLevel = 1;
	full.pBaseStr = (BYTE)hero_max_stat(FLAVOR_DIABLO, PC_WARRIOR, 0);
	full.pBaseMag = (BYTE)hero_max_stat(FLAVOR_DIABLO, PC_WARRIOR, 1);
	full.pBaseDex = (BYTE)hero_max_stat(FLAVOR_DIABLO, PC_WARRIOR, 2);
	full.pBaseVit = (BYTE)hero_max_stat(FLAVOR_DIABLO, PC_WARRIOR, 3);
	hero_level_up(&full, FLAVOR_DIABLO, 10, err);
	ok(full.pStatPts == 0,
	    "a character already at every cap is given no points it cannot spend");

	/* Going down is not levelling, and is refused as a no-op. */
	base_hero(&h, "Jazreth", 3);
	h.pLevel = 20;
	ok(hero_level_up(&h, FLAVOR_HELLFIRE, 10, err) == 0,
	    "lowering the level gains nothing");
	ok(h.pLevel == 20, "  ...and leaves the character alone");

	/* The sheet drives it through the slider. */
	Editor ed;
	base_hero(&h, "Jazreth", 3);
	h.pLevel = 3;
	h.pExperience = hero_exp_for_level(3);
	h.pStatPts = 0;
	ed.Load(make_entry("/tmp/single_0.hsv", h));
	ed.level_target = 12;
	ed.ApplyLevelTarget();
	ok(ed.statpts == 9 * 5, "moving the slider shows the points on the sheet");
	ok(ed.hp_max > HERO_TO_WHOLE(h.pMaxHPBase), "  ...and the life");

	/* Dragging up and back down lands exactly where it started. */
	ed.level_target = 3;
	ed.ApplyLevelTarget();
	ok(ed.statpts == 0 && ed.hp_max == HERO_TO_WHOLE(h.pMaxHPBase),
	    "  ...and moving it back undoes them exactly");

	/* Validation must name the real mechanism, not the old wrong one. */
	base_hero(&h, "Jazreth", 3);
	h.pLevel = 3;
	h.pExperience = hero_exp_for_level(20); /* the state that never worked */
	DiagList dl;
	dl_init(&dl);
	hero_check(&h, FLAVOR_HELLFIRE, &dl);
	int names_clamp = 0;
	for (int i = 0; i < dl.n; i++)
		if (strstr(dl.items[i].msg, "ValidatePlayer") != NULL)
			names_clamp = 1;
	ok(dl_count(&dl, DIAG_WARNING) > 0 && names_clamp,
	    "validation says the game will clamp it, not that it will be awarded");
	dl_free(&dl);
}

/* ------------------------------------------------------------------ */
/* 4. Name sanitising                                                  */
/* ------------------------------------------------------------------ */

static void check_names(void)
{
	section("4. the name field cannot hold what validation rejects");

	/* Return used to insert a newline, which then failed hero_name_valid. */
	ok(editor_sanitize_name("Aidan\n") == "Aidan", "a trailing newline is dropped");
	ok(editor_sanitize_name("Ai\ndan") == "Aidan", "an embedded newline is dropped");
	ok(editor_sanitize_name("Ai\tdan") == "Aidan", "a tab is dropped");
	ok(editor_sanitize_name("Aidan\r") == "Aidan", "a carriage return is dropped");
	ok(editor_sanitize_name("a/b") == "ab", "a slash is dropped");
	ok(editor_sanitize_name("a\\b") == "ab", "a backslash is dropped");
	ok(editor_sanitize_name("a:b") == "ab", "a colon is dropped");
	ok(editor_sanitize_name("Aidan") == "Aidan", "an ordinary name is untouched");
	ok(editor_sanitize_name("") == "", "an empty name stays empty");

	std::string too_long(80, 'x');
	ok((int)editor_sanitize_name(too_long).size() == PLR_NAME_LEN - 1,
	    "an over-long name is clamped to what the save holds");

	/* Whatever survives sanitising must pass validation. */
	char err[HERO_ERR_LEN];
	static const char *nasty[] = { "Aidan\n", "a/b\tc", "\r\n", "x:y\\z" };
	int all_ok = 1;
	for (size_t i = 0; i < sizeof(nasty) / sizeof(nasty[0]); i++) {
		std::string clean = editor_sanitize_name(nasty[i]);
		if (clean.empty())
			continue; /* an empty name is rejected, correctly */
		if (!hero_name_valid(clean.c_str(), err))
			all_ok = 0;
	}
	ok(all_ok, "anything the field accepts also passes hero_name_valid");
}

/* ------------------------------------------------------------------ */

int main(void)
{
	printf("butcher -- TUI editor model\n");

	check_addresses();
	check_roundtrip();
	check_caps();
	check_spell_flavor();
	check_repair();
	check_level_up();
	check_names();

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
