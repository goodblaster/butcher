/**
 * @file editor.h
 *
 * The character being edited, independent of how it is displayed.
 *
 * Split out of the view so its invariants can be tested without a terminal.
 * The one that matters: every field a UI widget binds to must live at a
 * *stable address* for the life of the program. FTXUI stores a raw pointer,
 * so a std::vector that Load() reassigns would leave widgets pointing at
 * freed memory -- which is exactly how the spell pane used to crash.
 */
#ifndef BUTCHER_TUI_EDITOR_H
#define BUTCHER_TUI_EDITOR_H

#include "../src/saveutil.h"

#include <string>

/*
 * FTXUI components bind to plain variables, so the character is mirrored into
 * scalars while editing and composed back on demand. Compose() is the single
 * place that turns the mirror into a PkPlayerStruct, so every view -- the
 * sheet, the diagnostics, the dirty flag, the save -- sees the same thing.
 */
struct Editor {
	SaveEntry entry;
	PkPlayerStruct original {};

	std::string name;
	int cls = 0;
	int str = 0, mag = 0, dex = 0, vit = 0, statpts = 0;
	int level_target = 1, level_opened = 1, level_stored = 1;
	int hp = 0, hp_max = 0, mana = 0, mana_max = 0;
	int gold = 0, gold_opened = 0, gold_cap = 0;
	int dlvl = 0;
	/*
	 * Fixed arrays, deliberately. FTXUI components bind to a raw address, and
	 * a std::vector that Load() reassigns can move -- leaving every spell
	 * slider pointing at freed memory. These never move.
	 */
	static const int kSpellSlots = 64; /* >= hero_spell_persisted() */
	int spell_lvl[kSpellSlots] = {};
	bool spell_known[kSpellSlots] = {};

	/* Bound to the sliders, so switching class moves the ceilings live. */
	int cap_str = 0, cap_mag = 0, cap_dex = 0, cap_vit = 0;
	int cap_dlvl = 0, cap_life = 0, cap_mana = 0, cap_gold = 0;

	void RefreshCaps()
	{
		HeroFlavor f = entry.flavor;
		cap_str = hero_max_stat(f, cls, ATTRIB_STR);
		cap_mag = hero_max_stat(f, cls, ATTRIB_MAG);
		cap_dex = hero_max_stat(f, cls, ATTRIB_DEX);
		cap_vit = hero_max_stat(f, cls, ATTRIB_VIT);
		cap_dlvl = hero_num_levels(f) - 1;
		cap_gold = gold_cap;
		/*
		 * Derived from the game's own formulas, so the bar means something --
		 * these were a flat 100000, against which a 123-life character drew
		 * no visible fill at all. An existing character can legitimately sit
		 * above the computed ceiling, since items are not in the base value,
		 * so never clamp downward.
		 */
		cap_life = hero_max_life(f, cls);
		cap_mana = hero_max_mana(f, cls);
		if (hp_max > cap_life)
			cap_life = hp_max;
		if (mana_max > cap_mana)
			cap_mana = mana_max;

		/* A class change can lower a ceiling under a value that was legal a
		 * moment ago; clamp rather than leave the sheet showing something the
		 * game would reject. */
		if (str > cap_str)
			str = cap_str;
		if (mag > cap_mag)
			mag = cap_mag;
		if (dex > cap_dex)
			dex = cap_dex;
		if (vit > cap_vit)
			vit = cap_vit;
		if (dlvl > cap_dlvl)
			dlvl = cap_dlvl;
	}

	void Load(const SaveEntry &e)
	{
		entry = e;
		original = e.hero;

		char n[PLR_NAME_LEN + 1];
		memcpy(n, e.hero.pName, PLR_NAME_LEN);
		n[PLR_NAME_LEN] = '\0';
		name = n;

		cls = e.hero.pClass;
		str = e.hero.pBaseStr;
		mag = e.hero.pBaseMag;
		dex = e.hero.pBaseDex;
		vit = e.hero.pBaseVit;
		statpts = e.hero.pStatPts;
		hp = HERO_TO_WHOLE(e.hero.pHPBase);
		hp_max = HERO_TO_WHOLE(e.hero.pMaxHPBase);
		mana = HERO_TO_WHOLE(e.hero.pManaBase);
		mana_max = HERO_TO_WHOLE(e.hero.pMaxManaBase);
		dlvl = e.hero.plrlevel;

		level_stored = e.hero.pLevel;
		level_target = hero_level_for_exp(e.hero.pExperience);
		level_opened = level_target;

		gold = hero_gold_in_stacks(&e.hero);
		gold_opened = gold;
		gold_cap = hero_gold_capacity(&e.hero);

		for (int s = 0; s < kSpellSlots; s++) {
			spell_lvl[s] = 0;
			spell_known[s] = false;
		}
		for (int s = 0; s < hero_spell_persisted(); s++) {
			spell_lvl[s] = hero_get_spell_level(&e.hero, s);
			spell_known[s] = s > 0 && (e.hero.pMemSpells & SPELLBIT(s)) != 0;
		}

		RefreshCaps();
	}

	PkPlayerStruct Compose() const
	{
		PkPlayerStruct h = original;

		memset(h.pName, 0, PLR_NAME_LEN);
		strncpy(h.pName, name.c_str(), PLR_NAME_LEN - 1);
		h.pClass = (char)cls;
		h.pBaseStr = (BYTE)str;
		h.pBaseMag = (BYTE)mag;
		h.pBaseDex = (BYTE)dex;
		h.pBaseVit = (BYTE)vit;
		h.pStatPts = (BYTE)statpts;
		h.pHPBase = HERO_FROM_WHOLE(hp) | (original.pHPBase & 63);
		h.pMaxHPBase = HERO_FROM_WHOLE(hp_max) | (original.pMaxHPBase & 63);
		h.pManaBase = HERO_FROM_WHOLE(mana) | (original.pManaBase & 63);
		h.pMaxManaBase = HERO_FROM_WHOLE(mana_max) | (original.pMaxManaBase & 63);
		h.plrlevel = (BYTE)dlvl;

		/*
		 * Only rewrite experience once the level slider has moved. Snapping to
		 * a level threshold discards progress within the level, so opening a
		 * character and touching nothing must leave it byte-identical.
		 */
		if (level_target != level_opened)
			h.pExperience = hero_exp_for_level(level_target);

		/*
		 * Only spells this save's game defines. The pane is built once and
		 * covers both flavors, so without this a Diablo character picks up
		 * Hellfire ids that the game would read past the end of its spell
		 * table -- which is what made an edited character unloadable.
		 */
		for (int s = 1; s < hero_spell_persisted(); s++) {
			char err[HERO_ERR_LEN];
			if (!hero_spell_exists(entry.flavor, s)) {
				if (s < MAX_SPELLS)
					h.pSplLvl[s] = 0;
				else
					h.pSplLvl2[s - MAX_SPELLS] = 0;
				h.pMemSpells &= ~SPELLBIT(s);
				continue;
			}
			hero_set_spell_level(&h, entry.flavor, s, spell_lvl[s], err);
			/* A spell with no book cannot sit in _pMemSpells; the game masks
			 * it straight back out. */
			if (spell_known[s] && hero_spell_has_book(entry.flavor, s))
				h.pMemSpells |= SPELLBIT(s);
			else
				h.pMemSpells &= ~SPELLBIT(s);
		}
		h.pSplLvl[0] = (char)spell_lvl[0];

		/* Likewise: hero_set_gold rewrites the inventory, so leave it alone
		 * unless the total actually changed. */
		if (gold != gold_opened) {
			char err[HERO_ERR_LEN];
			hero_set_gold(&h, gold, err);
		}
		return h;
	}

	bool Dirty() const
	{
		PkPlayerStruct h = Compose();
		return memcmp(&h, &original, sizeof(h)) != 0;
	}
};

/**
 * Strip anything a character name cannot hold and clamp the length.
 *
 * The UI must not accept what validation will reject: Return used to insert a
 * newline into the name field, which then failed hero_name_valid.
 */
std::string editor_sanitize_name(const std::string &in);

#endif /* BUTCHER_TUI_EDITOR_H */
