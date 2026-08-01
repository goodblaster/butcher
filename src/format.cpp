/**
 * @file format.cpp
 *
 * Human-readable rendering of a character. See format.h.
 */
#include "format.h"

#include "../third_party/devilution/Source/itemdat.h"

/* Hellfire's item table; see the note in hero.cpp. */
extern ItemDataStruct AllItemsList_hf[];

/* ------------------------------------------------------------------ */
/* Items                                                               */
/* ------------------------------------------------------------------ */

/*
 * AllItemsList has no published length; the game finds its end with a
 * sentinel entry whose iLoc is ILOC_INVALID (Source/items.cpp:2650). Use the
 * same convention rather than hardcoding a count that would rot.
 */
static int item_table_len(HeroFlavor f)
{
	static int len[2] = { -1, -1 };
	if (len[f] < 0) {
		const ItemDataStruct *t = (f == FLAVOR_HELLFIRE) ? AllItemsList_hf : AllItemsList;
		int n = 0;
		while (t[n].iLoc != ILOC_INVALID)
			n++;
		len[f] = n;
	}
	return len[f];
}

const char *format_item_name(HeroFlavor f, int idx)
{
	if (idx < 0 || idx >= item_table_len(f))
		return NULL;
	return (f == FLAVOR_HELLFIRE) ? AllItemsList_hf[idx].iName : AllItemsList[idx].iName;
}

/** Render one packed item slot into buf. */
static void describe_item(const PkItemStruct *it, HeroFlavor f, char *buf, size_t n)
{
	if (it->idx == 0xFFFF) {
		snprintf(buf, n, "-");
		return;
	}
	if (it->idx == IDI_GOLD) {
		snprintf(buf, n, "%u gold", it->wValue);
		return;
	}

	const char *name = format_item_name(f, it->idx);
	char extra[64] = { 0 };

	if (it->bMDur != 0)
		snprintf(extra, sizeof(extra), "  dur %u/%u", it->bDur, it->bMDur);
	else if (it->bMCh != 0)
		snprintf(extra, sizeof(extra), "  chg %u/%u", it->bCh, it->bMCh);

	/*
	 * The base name is all that is recoverable without replaying item
	 * generation: affixes live in the seed, not in the packed struct.
	 */
	snprintf(buf, n, "%s%s%s%s", name != NULL ? name : "unknown item",
	    (it->bId & 2) ? " (magic)" : "",
	    (it->bId & 1) ? "" : (it->bId & 2) ? ", unidentified" : "",
	    extra);
}

/* ------------------------------------------------------------------ */

void format_brief(const PkPlayerStruct *h, HeroFlavor f, FILE *out)
{
	char name[PLR_NAME_LEN + 1];
	memcpy(name, h->pName, PLR_NAME_LEN);
	name[PLR_NAME_LEN] = '\0';

	fprintf(out, "%-20s %-9s %-9s lvl %-3d %9d gold\n", name,
	    hero_flavor_name(f), hero_class_name(f, h->pClass), h->pLevel, h->pGold);
}

void format_show(const PkPlayerStruct *h, HeroFlavor f, FILE *out)
{
	char name[PLR_NAME_LEN + 1];
	char buf[128];

	memcpy(name, h->pName, PLR_NAME_LEN);
	name[PLR_NAME_LEN] = '\0';

	fprintf(out, "%s\n", name);
	fprintf(out, "  %s %s, level %d\n", hero_flavor_name(f),
	    hero_class_name(f, h->pClass), h->pLevel);
	fprintf(out, "  experience %d", h->pExperience);
	if (h->pLevel < hero_max_level())
		fprintf(out, " (next level at %d)", hero_exp_for_level(h->pLevel + 1));
	fprintf(out, "\n");
	fprintf(out, "  dungeon level %u", h->plrlevel);
	if (h->pDiabloKillLevel)
		fprintf(out, ", Diablo killed on difficulty %u", (unsigned)h->pDiabloKillLevel);
	fprintf(out, "\n");

	fprintf(out, "\nAttributes                     cap\n");
	static const char *labels[4] = { "strength", "magic", "dexterity", "vitality" };
	const BYTE vals[4] = { h->pBaseStr, h->pBaseMag, h->pBaseDex, h->pBaseVit };
	for (int i = 0; i < 4; i++)
		fprintf(out, "  %-12s %14u %5d\n", labels[i], vals[i],
		    hero_max_stat(f, h->pClass, i));
	fprintf(out, "  %-12s %14u\n", "unspent", h->pStatPts);

	fprintf(out, "\n  life         %10d / %-6d\n",
	    HERO_TO_WHOLE(h->pHPBase), HERO_TO_WHOLE(h->pMaxHPBase));
	fprintf(out, "  mana         %10d / %-6d\n",
	    HERO_TO_WHOLE(h->pManaBase), HERO_TO_WHOLE(h->pMaxManaBase));

	/* Gold, with the stack breakdown that determines the real total. */
	int stacked = hero_gold_in_stacks(h);
	fprintf(out, "\nGold\n");
	fprintf(out, "  cached total %10d\n", h->pGold);
	fprintf(out, "  in stacks    %10d%s\n", stacked,
	    stacked == h->pGold ? "" : "   <- the game trusts this one");
	int nstacks = 0;
	for (int i = 0; i < h->_pNumInv && i < NUM_INV_GRID_ELEM; i++)
		if (h->InvList[i].idx == IDI_GOLD)
			nstacks++;
	for (int i = 0; i < MAXBELTITEMS; i++)
		if (h->SpdList[i].idx == IDI_GOLD)
			nstacks++;
	fprintf(out, "  %d stack%s, %d of %d inventory cells free\n", nstacks,
	    nstacks == 1 ? "" : "s", hero_free_inv_cells(h), NUM_INV_GRID_ELEM);

	fprintf(out, "\nEquipped\n");
	static const char *slots[NUM_INVLOC] = {
		"head", "left ring", "right ring", "amulet",
		"left hand", "right hand", "chest"
	};
	for (int i = 0; i < NUM_INVLOC; i++) {
		describe_item(&h->InvBody[i], f, buf, sizeof(buf));
		fprintf(out, "  %-11s %s\n", slots[i], buf);
	}

	fprintf(out, "\nInventory (%u item%s)\n", h->_pNumInv,
	    h->_pNumInv == 1 ? "" : "s");
	int shown = 0;
	for (int i = 0; i < h->_pNumInv && i < NUM_INV_GRID_ELEM; i++) {
		describe_item(&h->InvList[i], f, buf, sizeof(buf));
		if (h->InvList[i].idx != 0xFFFF) {
			fprintf(out, "  %2d. %s\n", i + 1, buf);
			shown++;
		}
	}
	if (shown == 0)
		fprintf(out, "  (empty)\n");

	int belt = 0;
	for (int i = 0; i < MAXBELTITEMS; i++)
		if (h->SpdList[i].idx != 0xFFFF)
			belt++;
	if (belt > 0) {
		fprintf(out, "\nBelt\n");
		for (int i = 0; i < MAXBELTITEMS; i++) {
			if (h->SpdList[i].idx == 0xFFFF)
				continue;
			describe_item(&h->SpdList[i], f, buf, sizeof(buf));
			fprintf(out, "  %d. %s\n", i + 1, buf);
		}
	}

	/* Spells: known (pMemSpells bitmask) and per-spell levels. */
	fprintf(out, "\nSpells\n");
	int any = 0;
	for (int s = 1; s < hero_spell_persisted(); s++) {
		int known = (h->pMemSpells & SPELLBIT(s)) != 0;
		int lvl = hero_get_spell_level(h, s);
		if (!known && lvl == 0)
			continue;
		const char *sn = hero_spell_name(f, s);
		fprintf(out, "  %-18s level %-3d%s\n", sn != NULL ? sn : "?", lvl,
		    known ? "" : "   (not in spell book)");
		any = 1;
	}
	if (!any)
		fprintf(out, "  (none)\n");
}

/* ------------------------------------------------------------------ */
/* Diff                                                                */
/* ------------------------------------------------------------------ */

static int diff_int(const char *label, int a, int b, FILE *out)
{
	if (a == b)
		return 0;
	fprintf(out, "  %-12s %d -> %d\n", label, a, b);
	return 1;
}

int format_diff(const PkPlayerStruct *before, const PkPlayerStruct *after,
    HeroFlavor f, FILE *out)
{
	int n = 0;

	if (memcmp(before->pName, after->pName, PLR_NAME_LEN) != 0) {
		char a[PLR_NAME_LEN + 1], b[PLR_NAME_LEN + 1];
		memcpy(a, before->pName, PLR_NAME_LEN);
		a[PLR_NAME_LEN] = '\0';
		memcpy(b, after->pName, PLR_NAME_LEN);
		b[PLR_NAME_LEN] = '\0';
		fprintf(out, "  %-12s %s -> %s\n", "name", a, b);
		n++;
	}
	if (before->pClass != after->pClass) {
		fprintf(out, "  %-12s %s -> %s\n", "class",
		    hero_class_name(f, before->pClass), hero_class_name(f, after->pClass));
		n++;
	}
	n += diff_int("level", before->pLevel, after->pLevel, out);
	n += diff_int("experience", before->pExperience, after->pExperience, out);
	n += diff_int("statpts", before->pStatPts, after->pStatPts, out);
	n += diff_int("strength", before->pBaseStr, after->pBaseStr, out);
	n += diff_int("magic", before->pBaseMag, after->pBaseMag, out);
	n += diff_int("dexterity", before->pBaseDex, after->pBaseDex, out);
	n += diff_int("vitality", before->pBaseVit, after->pBaseVit, out);
	n += diff_int("life", HERO_TO_WHOLE(before->pHPBase),
	    HERO_TO_WHOLE(after->pHPBase), out);
	n += diff_int("max life", HERO_TO_WHOLE(before->pMaxHPBase),
	    HERO_TO_WHOLE(after->pMaxHPBase), out);
	n += diff_int("mana", HERO_TO_WHOLE(before->pManaBase),
	    HERO_TO_WHOLE(after->pManaBase), out);
	n += diff_int("max mana", HERO_TO_WHOLE(before->pMaxManaBase),
	    HERO_TO_WHOLE(after->pMaxManaBase), out);
	n += diff_int("gold", before->pGold, after->pGold, out);
	n += diff_int("dungeon lvl", before->plrlevel, after->plrlevel, out);

	for (int s = 1; s < hero_spell_persisted(); s++) {
		int lb = hero_get_spell_level(before, s), la = hero_get_spell_level(after, s);
		if (lb == la)
			continue;
		const char *sn = hero_spell_name(f, s);
		char label[32];
		snprintf(label, sizeof(label), "%s", sn != NULL ? sn : "spell");
		fprintf(out, "  %-16s level %d -> %d\n", label, lb, la);
		n++;
	}
	if (before->pMemSpells != after->pMemSpells) {
		fprintf(out, "  %-12s spell book changed\n", "spells");
		n++;
	}

	int gb = hero_gold_in_stacks(before), ga = hero_gold_in_stacks(after);
	if (gb != ga)
		fprintf(out, "  %-12s %d -> %d\n", "gold stacks", gb, ga);

	return n;
}
