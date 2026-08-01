/**
 * @file charjson.cpp
 *
 * Character <-> JSON. See charjson.h for the losslessness contract.
 */
#include "charjson.h"

#include "format.h" /* item names, for the informational "name" field */
#include "json.h"

#include <stdarg.h>
#include <strings.h> /* strcasecmp */

const char *const charjson_equip_slots[NUM_INVLOC] = {
	"head", "left_ring", "right_ring", "amulet", "left_hand", "right_hand", "chest"
};

/* Every spell id the format carries; ids above this cannot be saved at all. */
#define SPELL_IDS (MAX_SPELLS + 10) /* 47 */

/*
 * Bits of pMemSpells covered by the spells array. SPELLBIT(s) is 1 << (s-1),
 * so id 0 has no bit -- ids 1..46 occupy bits 0..45. Anything above that is
 * carried in advanced.mem_spells_extra_bits.
 */
static unsigned long long spell_bit_mask(void)
{
	unsigned long long m = 0;
	for (int s = 1; s < SPELL_IDS; s++)
		m |= SPELLBIT(s);
	return m;
}

/* ------------------------------------------------------------------ */
/* Write                                                              */
/* ------------------------------------------------------------------ */

static void write_item(JsonWriter *w, const char *key, const PkItemStruct *it,
    HeroFlavor f, int slot)
{
	if (it->idx == 0xFFFF) {
		if (key != NULL)
			jw_null(w, key);
		return;
	}

	jw_obj_open(w, key);
	if (slot >= 0)
		jw_int(w, "slot", slot);
	jw_int(w, "index", it->idx);

	/* Purely informational; import ignores it. The base name is all that is
	 * recoverable -- affixes live in the seed, not in these fields. */
	if (it->idx == IDI_GOLD) {
		jw_str(w, "name", "Gold");
	} else {
		const char *n = format_item_name(f, it->idx);
		jw_str(w, "name", n != NULL ? n : "unknown");
	}

	jw_int(w, "seed", (long long)(uint32_t)it->iSeed);
	jw_int(w, "create_info", it->iCreateInfo);
	jw_int(w, "id_flags", it->bId);
	jw_int(w, "durability", it->bDur);
	jw_int(w, "max_durability", it->bMDur);
	jw_int(w, "charges", it->bCh);
	jw_int(w, "max_charges", it->bMCh);
	jw_int(w, "value", it->wValue);
	jw_int(w, "buff", (long long)(uint32_t)it->dwBuff);
	jw_obj_close(w);
}

/** Life or mana: whole points, plus the 1/64 remainder only when nonzero. */
static void write_pair(JsonWriter *w, const char *key, int cur, int max)
{
	jw_obj_open(w, key);
	jw_int(w, "current", cur >> HERO_FIXED_SHIFT);
	jw_int(w, "max", max >> HERO_FIXED_SHIFT);
	if ((cur & 63) != 0)
		jw_int(w, "current_64ths", cur & 63);
	if ((max & 63) != 0)
		jw_int(w, "max_64ths", max & 63);
	jw_obj_close(w);
}

char *charjson_write(const PkPlayerStruct *h, HeroFlavor f, void (*warn)(const char *))
{
	JsonWriter *w = jw_new();
	char name[PLR_NAME_LEN + 1];

	memcpy(name, h->pName, PLR_NAME_LEN);
	name[PLR_NAME_LEN] = '\0';

	/* The one documented gap in losslessness. */
	if (warn != NULL) {
		size_t used = strlen(name);
		for (size_t i = used + 1; i < PLR_NAME_LEN; i++) {
			if (h->pName[i] != '\0') {
				warn("the name field has nonzero bytes after its terminator; "
				     "those are not carried in the JSON and will import as zero");
				break;
			}
		}
	}

	jw_obj_open(w, NULL);

	jw_str(w, "format", "devilution-character");
	jw_int(w, "version", BUTCHER_FORMAT_VERSION);
	jw_str(w, "game", f == FLAVOR_HELLFIRE ? "hellfire" : "diablo");

	jw_gap(w);
	jw_str(w, "name", name);
	{
		const char *cn = hero_class_name(f, h->pClass);
		if (strcmp(cn, "unknown") == 0)
			jw_int(w, "class", h->pClass); /* keep the number when unnamed */
		else
			jw_str(w, "class", cn);
	}
	jw_int(w, "level", h->pLevel);
	jw_int(w, "experience", h->pExperience);
	jw_int(w, "stat_points", h->pStatPts);

	jw_gap(w);
	jw_obj_open(w, "attributes");
	jw_int(w, "strength", h->pBaseStr);
	jw_int(w, "magic", h->pBaseMag);
	jw_int(w, "dexterity", h->pBaseDex);
	jw_int(w, "vitality", h->pBaseVit);
	jw_obj_close(w);

	jw_gap(w);
	write_pair(w, "life", h->pHPBase, h->pMaxHPBase);
	write_pair(w, "mana", h->pManaBase, h->pMaxManaBase);

	jw_gap(w);
	/* The cached total. The stacks in `inventory` are what the game trusts. */
	jw_int(w, "gold", h->pGold);

	jw_gap(w);
	jw_obj_open(w, "position");
	jw_int(w, "dungeon_level", h->plrlevel);
	jw_int(w, "x", h->px);
	jw_int(w, "y", h->py);
	jw_int(w, "target_x", h->targx);
	jw_int(w, "target_y", h->targy);
	jw_obj_close(w);

	jw_gap(w);
	jw_obj_open(w, "progress");
	jw_int(w, "diablo_kill_level", (long long)(uint32_t)h->pDiabloKillLevel);
	jw_int(w, "difficulty", h->pDifficulty);
	jw_int(w, "town_warps", h->pTownWarps);
	jw_int(w, "dungeon_messages", h->pDungMsgs);
	jw_int(w, "level_load", h->pLvlLoad);
	jw_bool_or_int(w, "mana_shield", h->pManaShield);
	jw_int(w, "reflections", h->wReflections);
	jw_int(w, "damage_ac_flags", h->pDamAcFlags);
	/*
	 * Same byte, different meaning per flavor (pBattleNet vs pDungMsgs2).
	 * Written under the name that applies; import accepts either.
	 */
	if (f == FLAVOR_HELLFIRE)
		jw_int(w, "dungeon_messages2", (unsigned char)h->pBattleNet);
	else
		jw_int(w, "battle_net", h->pBattleNet);
	jw_obj_close(w);

	jw_gap(w);
	jw_arr_open(w, "spells");
	for (int s = 0; s < SPELL_IDS; s++) {
		int lvl = hero_get_spell_level(h, s);
		/* Id 0 is SPL_NULL and has no spell-book bit. */
		int known = s > 0 && (h->pMemSpells & SPELLBIT(s)) != 0;
		if (lvl == 0 && !known)
			continue;
		jw_obj_open(w, NULL);
		jw_int(w, "id", s);
		const char *sn = hero_spell_name(f, s);
		if (sn != NULL)
			jw_str(w, "name", sn);
		jw_int(w, "level", lvl);
		if (s > 0)
			jw_bool(w, "in_book", known);
		jw_obj_close(w);
	}
	jw_arr_close(w);

	jw_gap(w);
	jw_obj_open(w, "equipment");
	for (int i = 0; i < NUM_INVLOC; i++)
		write_item(w, charjson_equip_slots[i], &h->InvBody[i], f, -1);
	jw_obj_close(w);

	jw_gap(w);
	jw_obj_open(w, "inventory");
	jw_int(w, "count", h->_pNumInv);
	jw_arr_open(w, "items");
	for (int i = 0; i < NUM_INV_GRID_ELEM; i++)
		if (h->InvList[i].idx != 0xFFFF)
			write_item(w, NULL, &h->InvList[i], f, i);
	jw_arr_close(w);
	/* The 10x4 grid, one row per line, so it reads like the real thing.
	 * Values are 1-based InvList indices; negative marks a continuation cell. */
	jw_arr_open(w, "grid");
	for (int row = 0; row < 4; row++) {
		jw_arr_open_inline(w, NULL);
		for (int col = 0; col < 10; col++)
			jw_int(w, NULL, h->InvGrid[row * 10 + col]);
		jw_arr_close_inline(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);

	jw_gap(w);
	jw_arr_open(w, "belt");
	for (int i = 0; i < MAXBELTITEMS; i++)
		if (h->SpdList[i].idx != 0xFFFF)
			write_item(w, NULL, &h->SpdList[i], f, i);
	jw_arr_close(w);

	/*
	 * Everything the game keeps but nobody edits. Present so that an export
	 * followed by an import reproduces the save byte for byte.
	 */
	jw_gap(w);
	jw_obj_open(w, "advanced");
	jw_str(w, "_note", "transient and reserved fields; preserved verbatim, "
	                   "not meant to be edited");
	jw_int(w, "dest_action", h->destAction);
	jw_int(w, "dest_param1", h->destParam1);
	jw_int(w, "dest_param2", h->destParam2);
	jw_int(w, "archive_time_low", (long long)(uint32_t)h->archiveTime.dwLowDateTime);
	jw_int(w, "archive_time_high", (long long)(uint32_t)h->archiveTime.dwHighDateTime);
	jw_arr_open_inline(w, "reserved_bytes");
	for (int i = 0; i < 3; i++)
		jw_int(w, NULL, h->bReserved[i]);
	jw_arr_close_inline(w);
	jw_int(w, "reserved2", h->wReserved2);
	jw_int(w, "reserved8", h->wReserved8);
	jw_arr_open_inline(w, "reserved_dwords");
	for (int i = 0; i < 5; i++)
		jw_int(w, NULL, h->dwReserved[i]);
	jw_arr_close_inline(w);
	{
		/* Bits of pMemSpells outside the ids the spells array covers. */
		unsigned long long extra = h->pMemSpells & ~spell_bit_mask();
		if (extra != 0)
			jw_int(w, "mem_spells_extra_bits", (long long)extra);
	}
	jw_obj_close(w);

	jw_obj_close(w);

	/* One trailing newline, so the file is well-formed for text tools. */
	char *out = (char *)malloc(jw_len(w) + 2);
	memcpy(out, jw_text(w), jw_len(w));
	out[jw_len(w)] = '\n';
	out[jw_len(w) + 1] = '\0';
	jw_free(w);
	return out;
}

/* ------------------------------------------------------------------ */
/* Read                                                               */
/* ------------------------------------------------------------------ */

static void read_item(const JValue *v, PkItemStruct *it)
{
	memset(it, 0, sizeof(*it));
	if (v == NULL || jv_type(v) != JOBJ) {
		it->idx = 0xFFFF;
		return;
	}
	it->idx = (WORD)jv_get_int(v, "index", 0xFFFF);
	it->iSeed = (DWORD)jv_get_int(v, "seed", 0);
	it->iCreateInfo = (WORD)jv_get_int(v, "create_info", 0);
	it->bId = (BYTE)jv_get_int(v, "id_flags", 0);
	it->bDur = (BYTE)jv_get_int(v, "durability", 0);
	it->bMDur = (BYTE)jv_get_int(v, "max_durability", 0);
	it->bCh = (BYTE)jv_get_int(v, "charges", 0);
	it->bMCh = (BYTE)jv_get_int(v, "max_charges", 0);
	it->wValue = (WORD)jv_get_int(v, "value", 0);
	it->dwBuff = (DWORD)jv_get_int(v, "buff", 0);
}

static void read_pair(const JValue *v, int *cur, int *max)
{
	long long c = jv_get_int(v, "current", 0);
	long long m = jv_get_int(v, "max", 0);
	*cur = (int)((c << HERO_FIXED_SHIFT) | (jv_get_int(v, "current_64ths", 0) & 63));
	*max = (int)((m << HERO_FIXED_SHIFT) | (jv_get_int(v, "max_64ths", 0) & 63));
}

/**
 * Resolve "class" whether written as a name or a number.
 *
 * Returns success separately from the value: pClass is a signed char, so any
 * value from -128 to 127 is legitimate data and none of them can double as an
 * error sentinel.
 */
static int read_class(const JValue *root, int *out, char *err)
{
	const JValue *c = jv_get(root, "class");

	if (c == NULL) {
		*out = 0;
		return 1;
	}
	if (jv_type(c) == JINT) {
		long long v = jv_int(c, 0);
		if (v < -128 || v > 127) {
			snprintf(err, JSON_ERR_LEN,
			    "class %lld does not fit the one byte the save has for it", v);
			return 0;
		}
		*out = (int)v;
		return 1;
	}

	const char *s = jv_str(c, NULL);
	if (s == NULL) {
		snprintf(err, JSON_ERR_LEN, "\"class\" must be a name or a number");
		return 0;
	}

	/* Accept either flavor's names, so a mislabeled document still loads and
	 * then fails validation with a message about the class. */
	for (int flavor = 0; flavor < 2; flavor++)
		for (int i = 0; i < hero_num_classes((HeroFlavor)flavor); i++)
			if (strcasecmp(hero_class_name((HeroFlavor)flavor, i), s) == 0) {
				*out = i;
				return 1;
			}

	snprintf(err, JSON_ERR_LEN, "unknown class \"%s\"", s);
	return 0;
}

int charjson_read(const char *text, PkPlayerStruct *out, HeroFlavor *flavor, char *err)
{
	char perr_buf[JSON_ERR_LEN] = { 0 };

	/* Every failure below writes a reason; make an empty one impossible. */
	snprintf(err, JSON_ERR_LEN, "malformed character document");
	JValue *root = json_parse(text, perr_buf);

	if (root == NULL) {
		snprintf(err, JSON_ERR_LEN, "%s", perr_buf);
		return 0;
	}
	if (jv_type(root) != JOBJ) {
		snprintf(err, JSON_ERR_LEN, "the document must be a JSON object");
		jv_free(root);
		return 0;
	}

	const char *fmt = jv_get_str(root, "format", NULL);
	if (fmt != NULL && strcmp(fmt, "devilution-character") != 0) {
		snprintf(err, JSON_ERR_LEN,
		    "\"format\" is \"%s\"; expected \"devilution-character\"", fmt);
		jv_free(root);
		return 0;
	}
	long long ver = jv_get_int(root, "version", BUTCHER_FORMAT_VERSION);
	if (ver > BUTCHER_FORMAT_VERSION) {
		snprintf(err, JSON_ERR_LEN,
		    "document version %lld is newer than this tool understands (%d)",
		    ver, BUTCHER_FORMAT_VERSION);
		jv_free(root);
		return 0;
	}

	HeroFlavor f = FLAVOR_DIABLO;
	const char *game = jv_get_str(root, "game", NULL);
	if (game != NULL) {
		if (strcasecmp(game, "hellfire") == 0)
			f = FLAVOR_HELLFIRE;
		else if (strcasecmp(game, "diablo") != 0) {
			snprintf(err, JSON_ERR_LEN,
			    "\"game\" is \"%s\"; expected \"diablo\" or \"hellfire\"", game);
			jv_free(root);
			return 0;
		}
	}
	if (flavor != NULL)
		*flavor = f;

	PkPlayerStruct h;
	memset(&h, 0, sizeof(h));

	const char *name = jv_get_str(root, "name", "");
	if (strlen(name) >= PLR_NAME_LEN) {
		snprintf(err, JSON_ERR_LEN, "name is %zu characters; the limit is %d",
		    strlen(name), PLR_NAME_LEN - 1);
		jv_free(root);
		return 0;
	}
	strncpy(h.pName, name, PLR_NAME_LEN - 1);

	int cls = 0;
	if (!read_class(root, &cls, err)) {
		jv_free(root);
		return 0;
	}
	h.pClass = (char)cls;

	h.pLevel = (char)jv_get_int(root, "level", 0);
	h.pExperience = (int)jv_get_int(root, "experience", 0);
	h.pStatPts = (BYTE)jv_get_int(root, "stat_points", 0);
	h.pGold = (int)jv_get_int(root, "gold", 0);

	const JValue *a = jv_get(root, "attributes");
	h.pBaseStr = (BYTE)jv_get_int(a, "strength", 0);
	h.pBaseMag = (BYTE)jv_get_int(a, "magic", 0);
	h.pBaseDex = (BYTE)jv_get_int(a, "dexterity", 0);
	h.pBaseVit = (BYTE)jv_get_int(a, "vitality", 0);

	read_pair(jv_get(root, "life"), &h.pHPBase, &h.pMaxHPBase);
	read_pair(jv_get(root, "mana"), &h.pManaBase, &h.pMaxManaBase);

	const JValue *p = jv_get(root, "position");
	h.plrlevel = (BYTE)jv_get_int(p, "dungeon_level", 0);
	h.px = (BYTE)jv_get_int(p, "x", 0);
	h.py = (BYTE)jv_get_int(p, "y", 0);
	h.targx = (BYTE)jv_get_int(p, "target_x", 0);
	h.targy = (BYTE)jv_get_int(p, "target_y", 0);

	const JValue *pr = jv_get(root, "progress");
	h.pDiabloKillLevel = (DWORD)jv_get_int(pr, "diablo_kill_level", 0);
	h.pDifficulty = (int)jv_get_int(pr, "difficulty", 0);
	h.pTownWarps = (char)jv_get_int(pr, "town_warps", 0);
	h.pDungMsgs = (char)jv_get_int(pr, "dungeon_messages", 0);
	h.pLvlLoad = (char)jv_get_int(pr, "level_load", 0);
	h.pManaShield = (BOOLEAN)jv_get_int(pr, "mana_shield", 0);
	h.wReflections = (WORD)jv_get_int(pr, "reflections", 0);
	h.pDamAcFlags = (int)jv_get_int(pr, "damage_ac_flags", 0);
	/* Whichever name the document used for the shared byte. */
	if (jv_get(pr, "dungeon_messages2") != NULL)
		h.pBattleNet = (char)jv_get_int(pr, "dungeon_messages2", 0);
	else
		h.pBattleNet = (char)jv_get_int(pr, "battle_net", 0);

	const JValue *sp = jv_get(root, "spells");
	for (int i = 0; i < jv_count(sp); i++) {
		const JValue *e = jv_at(sp, i);
		long long id = jv_get_int(e, "id", -1);
		if (id < 0 || id >= SPELL_IDS) {
			snprintf(err, JSON_ERR_LEN,
			    "spells[%d]: id %lld is outside the range a save can hold (0..%d)",
			    i, id, SPELL_IDS - 1);
			jv_free(root);
			return 0;
		}
		int lvl = (int)jv_get_int(e, "level", 0);
		if (id < MAX_SPELLS)
			h.pSplLvl[id] = (char)lvl;
		else
			h.pSplLvl2[id - MAX_SPELLS] = (char)lvl;
		if (id > 0 && jv_get_bool(e, "in_book", lvl > 0))
			h.pMemSpells |= SPELLBIT((int)id);
	}

	const JValue *eq = jv_get(root, "equipment");
	for (int i = 0; i < NUM_INVLOC; i++)
		read_item(jv_get(eq, charjson_equip_slots[i]), &h.InvBody[i]);

	for (int i = 0; i < NUM_INV_GRID_ELEM; i++) {
		memset(&h.InvList[i], 0, sizeof(h.InvList[i]));
		h.InvList[i].idx = 0xFFFF;
	}
	const JValue *inv = jv_get(root, "inventory");
	h._pNumInv = (BYTE)jv_get_int(inv, "count", 0);
	const JValue *items = jv_get(inv, "items");
	for (int i = 0; i < jv_count(items); i++) {
		const JValue *e = jv_at(items, i);
		long long slot = jv_get_int(e, "slot", i);
		if (slot < 0 || slot >= NUM_INV_GRID_ELEM) {
			snprintf(err, JSON_ERR_LEN,
			    "inventory.items[%d]: slot %lld is outside 0..%d", i, slot,
			    NUM_INV_GRID_ELEM - 1);
			jv_free(root);
			return 0;
		}
		read_item(e, &h.InvList[slot]);
	}

	/* Grid accepted either as 4 rows of 10 or as a flat 40. */
	const JValue *grid = jv_get(inv, "grid");
	if (jv_count(grid) == 4 && jv_type(jv_at(grid, 0)) == JARR) {
		for (int row = 0; row < 4; row++) {
			const JValue *r = jv_at(grid, row);
			for (int col = 0; col < 10 && col < jv_count(r); col++)
				h.InvGrid[row * 10 + col] = (char)jv_int(jv_at(r, col), 0);
		}
	} else {
		for (int i = 0; i < NUM_INV_GRID_ELEM && i < jv_count(grid); i++)
			h.InvGrid[i] = (char)jv_int(jv_at(grid, i), 0);
	}

	for (int i = 0; i < MAXBELTITEMS; i++) {
		memset(&h.SpdList[i], 0, sizeof(h.SpdList[i]));
		h.SpdList[i].idx = 0xFFFF;
	}
	const JValue *belt = jv_get(root, "belt");
	for (int i = 0; i < jv_count(belt); i++) {
		const JValue *e = jv_at(belt, i);
		long long slot = jv_get_int(e, "slot", i);
		if (slot < 0 || slot >= MAXBELTITEMS) {
			snprintf(err, JSON_ERR_LEN, "belt[%d]: slot %lld is outside 0..%d",
			    i, slot, MAXBELTITEMS - 1);
			jv_free(root);
			return 0;
		}
		read_item(e, &h.SpdList[slot]);
	}

	const JValue *adv = jv_get(root, "advanced");
	h.destAction = (char)jv_get_int(adv, "dest_action", 0);
	h.destParam1 = (char)jv_get_int(adv, "dest_param1", 0);
	h.destParam2 = (char)jv_get_int(adv, "dest_param2", 0);
	h.archiveTime.dwLowDateTime = (DWORD)jv_get_int(adv, "archive_time_low", 0);
	h.archiveTime.dwHighDateTime = (DWORD)jv_get_int(adv, "archive_time_high", 0);
	const JValue *rb = jv_get(adv, "reserved_bytes");
	for (int i = 0; i < 3 && i < jv_count(rb); i++)
		h.bReserved[i] = (char)jv_int(jv_at(rb, i), 0);
	h.wReserved2 = (short)jv_get_int(adv, "reserved2", 0);
	h.wReserved8 = (short)jv_get_int(adv, "reserved8", 0);
	const JValue *rd = jv_get(adv, "reserved_dwords");
	for (int i = 0; i < 5 && i < jv_count(rd); i++)
		h.dwReserved[i] = (int)jv_int(jv_at(rd, i), 0);
	h.pMemSpells |= (unsigned long long)jv_get_int(adv, "mem_spells_extra_bits", 0);

	*out = h;
	jv_free(root);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

/*
 * One table per object shape, serving two purposes at once: it says which keys
 * exist (so anything else is a typo) and what range each value must fit (so a
 * value that would be silently truncated on the way into the struct is caught
 * instead). Without the range half, "strength": 300 imports as 44.
 */

typedef enum FKind {
	F_INT,
	F_STR,
	F_BOOL_OR_INT, /* written as true/false, but any byte is legal */
	F_OBJ,
	F_ARR,
	F_ANY
} FKind;

typedef struct FSpec {
	const char *key;
	FKind kind;
	long long lo, hi;
} FSpec;

#define R_BYTE 0, 255
#define R_CHAR (-128), 127
#define R_WORD 0, 65535
#define R_SHORT (-32768), 32767
#define R_INT (-2147483647LL - 1), 2147483647LL
#define R_DWORD 0, 4294967295LL
#define R_NONE 0, 0

/* Life and mana are shifted left by 6 on the way in, so the whole-point value
 * has to leave room for that. */
#define R_FIXED_WHOLE (-33554432LL), 33554431LL

static const FSpec spec_attrs[] = {
	{ "strength", F_INT, R_BYTE },
	{ "magic", F_INT, R_BYTE },
	{ "dexterity", F_INT, R_BYTE },
	{ "vitality", F_INT, R_BYTE },
	{ NULL, F_ANY, R_NONE }
};

static const FSpec spec_pair[] = {
	{ "current", F_INT, R_FIXED_WHOLE },
	{ "max", F_INT, R_FIXED_WHOLE },
	{ "current_64ths", F_INT, 0, 63 },
	{ "max_64ths", F_INT, 0, 63 },
	{ NULL, F_ANY, R_NONE }
};

static const FSpec spec_pos[] = {
	{ "dungeon_level", F_INT, R_BYTE },
	{ "x", F_INT, R_BYTE },
	{ "y", F_INT, R_BYTE },
	{ "target_x", F_INT, R_BYTE },
	{ "target_y", F_INT, R_BYTE },
	{ NULL, F_ANY, R_NONE }
};

static const FSpec spec_prog[] = {
	{ "diablo_kill_level", F_INT, R_DWORD },
	{ "difficulty", F_INT, R_INT },
	{ "town_warps", F_INT, R_CHAR },
	{ "dungeon_messages", F_INT, R_CHAR },
	{ "level_load", F_INT, R_CHAR },
	{ "mana_shield", F_BOOL_OR_INT, R_BYTE },
	{ "reflections", F_INT, R_WORD },
	{ "damage_ac_flags", F_INT, R_INT },
	/* One byte, named for whichever game wrote it. */
	{ "battle_net", F_INT, (-128), 255 },
	{ "dungeon_messages2", F_INT, (-128), 255 },
	{ NULL, F_ANY, R_NONE }
};

static const FSpec spec_spell[] = {
	{ "id", F_INT, 0, SPELL_IDS - 1 },
	{ "name", F_STR, R_NONE },
	{ "level", F_INT, R_CHAR },
	{ "in_book", F_BOOL_OR_INT, 0, 1 },
	{ NULL, F_ANY, R_NONE }
};

static const FSpec spec_item[] = {
	{ "slot", F_INT, 0, NUM_INV_GRID_ELEM - 1 },
	{ "index", F_INT, R_WORD },
	{ "name", F_STR, R_NONE },
	{ "seed", F_INT, R_DWORD },
	{ "create_info", F_INT, R_WORD },
	{ "id_flags", F_INT, R_BYTE },
	{ "durability", F_INT, R_BYTE },
	{ "max_durability", F_INT, R_BYTE },
	{ "charges", F_INT, R_BYTE },
	{ "max_charges", F_INT, R_BYTE },
	{ "value", F_INT, R_WORD },
	{ "buff", F_INT, R_DWORD },
	{ NULL, F_ANY, R_NONE }
};

static const FSpec spec_inv[] = {
	{ "count", F_INT, 0, NUM_INV_GRID_ELEM },
	{ "items", F_ARR, R_NONE },
	{ "grid", F_ARR, R_NONE },
	{ NULL, F_ANY, R_NONE }
};

static const FSpec spec_adv[] = {
	{ "_note", F_STR, R_NONE },
	{ "dest_action", F_INT, R_CHAR },
	{ "dest_param1", F_INT, R_CHAR },
	{ "dest_param2", F_INT, R_CHAR },
	{ "archive_time_low", F_INT, R_DWORD },
	{ "archive_time_high", F_INT, R_DWORD },
	{ "reserved_bytes", F_ARR, R_NONE },
	{ "reserved2", F_INT, R_SHORT },
	{ "reserved8", F_INT, R_SHORT },
	{ "reserved_dwords", F_ARR, R_NONE },
	{ "mem_spells_extra_bits", F_INT, R_INT },
	{ NULL, F_ANY, R_NONE }
};

static const FSpec spec_root[] = {
	{ "format", F_STR, R_NONE },
	{ "version", F_INT, 1, BUTCHER_FORMAT_VERSION },
	{ "game", F_STR, R_NONE },
	{ "name", F_STR, R_NONE },
	{ "class", F_ANY, R_NONE }, /* a name or a number */
	{ "level", F_INT, R_CHAR },
	{ "experience", F_INT, R_INT },
	{ "stat_points", F_INT, R_BYTE },
	{ "attributes", F_OBJ, R_NONE },
	{ "life", F_OBJ, R_NONE },
	{ "mana", F_OBJ, R_NONE },
	{ "gold", F_INT, R_INT },
	{ "position", F_OBJ, R_NONE },
	{ "progress", F_OBJ, R_NONE },
	{ "spells", F_ARR, R_NONE },
	{ "equipment", F_OBJ, R_NONE },
	{ "inventory", F_OBJ, R_NONE },
	{ "belt", F_ARR, R_NONE },
	{ "advanced", F_OBJ, R_NONE },
	{ NULL, F_ANY, R_NONE }
};

static const char *kind_name(FKind k)
{
	switch (k) {
	case F_INT:
		return "a number";
	case F_STR:
		return "a string";
	case F_BOOL_OR_INT:
		return "true, false, or a number";
	case F_OBJ:
		return "an object";
	case F_ARR:
		return "an array";
	default:
		return "a value";
	}
}

static void join(char *out, size_t n, const char *base, const char *leaf)
{
	if (base == NULL || base[0] == '\0')
		snprintf(out, n, "%s", leaf);
	else
		snprintf(out, n, "%s.%s", base, leaf);
}

/** Check one object's members against @p spec, reporting unknown keys. */
static void check_obj(const JValue *v, const FSpec *spec, const char *path, DiagList *dl)
{
	if (v == NULL)
		return;
	if (jv_type(v) != JOBJ) {
		dl_add(dl, DIAG_ERROR, path, "expected an object, found %s",
		    kind_name(F_ANY));
		return;
	}

	/* Candidate list for spelling suggestions. */
	const char *cands[40];
	int ncands = 0;
	for (const FSpec *f = spec; f->key != NULL && ncands < 40; f++)
		cands[ncands++] = f->key;

	for (int i = 0; i < jv_count(v); i++) {
		const char *key = jv_key_at(v, i);
		const JValue *val = jv_member_at(v, i);

		const FSpec *found = NULL;
		for (const FSpec *f = spec; f->key != NULL; f++) {
			if (strcmp(f->key, key) == 0) {
				found = f;
				break;
			}
		}

		char where[DIAG_WHERE_LEN];
		join(where, sizeof(where), path, key);

		if (found == NULL) {
			const char *near = dl_nearest(key, cands, ncands);
			if (near != NULL)
				dl_add(dl, DIAG_ERROR, where,
				    "unknown field \"%s\" -- did you mean \"%s\"? As written it "
				    "is ignored, and \"%s\" imports as zero",
				    key, near, near);
			else
				dl_add(dl, DIAG_ERROR, where,
				    "unknown field \"%s\"; it would be ignored on import", key);
			continue;
		}

		/* Type. */
		JType t = jv_type(val);
		int type_ok = 1;
		switch (found->kind) {
		case F_INT:
			type_ok = (t == JINT);
			break;
		case F_STR:
			type_ok = (t == JSTR);
			break;
		case F_BOOL_OR_INT:
			type_ok = (t == JBOOL || t == JINT);
			break;
		case F_OBJ:
			type_ok = (t == JOBJ);
			break;
		case F_ARR:
			type_ok = (t == JARR);
			break;
		case F_ANY:
			type_ok = 1;
			break;
		}
		if (!type_ok) {
			dl_add(dl, DIAG_ERROR, where, "expected %s", kind_name(found->kind));
			continue;
		}

		/* Range, for anything numeric with a declared one. */
		if ((found->kind == F_INT || found->kind == F_BOOL_OR_INT)
		    && !(found->lo == 0 && found->hi == 0)) {
			long long n = jv_int(val, 0);
			if (n < found->lo || n > found->hi)
				dl_add(dl, DIAG_ERROR, where,
				    "%lld is outside %lld..%lld; the save stores this field in "
				    "too few bits and the value would be truncated",
				    n, found->lo, found->hi);
		}
	}
}

/** An array whose elements are all plain integers in a range. */
static void check_int_array(const JValue *v, const char *path, int expect_len,
    long long lo, long long hi, DiagList *dl)
{
	if (v == NULL)
		return;
	if (jv_type(v) != JARR) {
		dl_add(dl, DIAG_ERROR, path, "expected an array");
		return;
	}
	if (expect_len > 0 && jv_count(v) != expect_len)
		dl_add(dl, DIAG_WARNING, path,
		    "has %d entries; %d expected, and the rest default to zero",
		    jv_count(v), expect_len);

	for (int i = 0; i < jv_count(v); i++) {
		char where[DIAG_WHERE_LEN];
		snprintf(where, sizeof(where), "%s[%d]", path, i);
		const JValue *e = jv_at(v, i);
		if (jv_type(e) != JINT) {
			dl_add(dl, DIAG_ERROR, where, "expected a number");
			continue;
		}
		long long n = jv_int(e, 0);
		if (n < lo || n > hi)
			dl_add(dl, DIAG_ERROR, where, "%lld is outside %lld..%lld", n, lo, hi);
	}
}

void charjson_check(const char *text, DiagList *dl)
{
	char perr_buf[JSON_ERR_LEN] = { 0 };
	JValue *root = json_parse(text, perr_buf);

	if (root == NULL) {
		dl_add(dl, DIAG_ERROR, NULL, "%s", perr_buf);
		return;
	}
	if (jv_type(root) != JOBJ) {
		dl_add(dl, DIAG_ERROR, NULL, "the document must be a JSON object");
		jv_free(root);
		return;
	}

	check_obj(root, spec_root, "", dl);

	/* Identity fields with meanings beyond their type. */
	const char *fmt = jv_get_str(root, "format", NULL);
	if (fmt == NULL)
		dl_add(dl, DIAG_WARNING, "format",
		    "missing; a character document should carry "
		    "\"format\": \"devilution-character\"");
	else if (strcmp(fmt, "devilution-character") != 0)
		dl_add(dl, DIAG_ERROR, "format",
		    "is \"%s\"; expected \"devilution-character\"", fmt);

	const char *game = jv_get_str(root, "game", NULL);
	if (game != NULL && strcasecmp(game, "diablo") != 0
	    && strcasecmp(game, "hellfire") != 0)
		dl_add(dl, DIAG_ERROR, "game",
		    "is \"%s\"; expected \"diablo\" or \"hellfire\"", game);

	HeroFlavor f = (game != NULL && strcasecmp(game, "hellfire") == 0)
	    ? FLAVOR_HELLFIRE
	    : FLAVOR_DIABLO;

	const char *name = jv_get_str(root, "name", NULL);
	if (name != NULL && strlen(name) >= PLR_NAME_LEN)
		dl_add(dl, DIAG_ERROR, "name",
		    "is %zu characters; the save holds %d plus a terminator",
		    strlen(name), PLR_NAME_LEN - 1);

	{
		const JValue *c = jv_get(root, "class");
		const char *cs = jv_str(c, NULL);
		if (cs != NULL) {
			int known = 0;
			const char *cands[6];
			int nc = 0;
			for (int fl = 0; fl < 2; fl++)
				for (int i = 0; i < hero_num_classes((HeroFlavor)fl); i++) {
					const char *cn = hero_class_name((HeroFlavor)fl, i);
					if (strcasecmp(cn, cs) == 0)
						known = 1;
					if (fl == 1 && nc < 6)
						cands[nc++] = cn;
				}
			if (!known) {
				const char *near = dl_nearest(cs, cands, nc);
				if (near != NULL)
					dl_add(dl, DIAG_ERROR, "class",
					    "unknown class \"%s\" -- did you mean \"%s\"?", cs, near);
				else
					dl_add(dl, DIAG_ERROR, "class", "unknown class \"%s\"", cs);
			}
		} else if (c != NULL && jv_type(c) != JINT) {
			dl_add(dl, DIAG_ERROR, "class", "expected a class name or a number");
		}
	}

	check_obj(jv_get(root, "attributes"), spec_attrs, "attributes", dl);
	check_obj(jv_get(root, "life"), spec_pair, "life", dl);
	check_obj(jv_get(root, "mana"), spec_pair, "mana", dl);
	check_obj(jv_get(root, "position"), spec_pos, "position", dl);
	check_obj(jv_get(root, "progress"), spec_prog, "progress", dl);
	check_obj(jv_get(root, "advanced"), spec_adv, "advanced", dl);

	/* Spells. */
	const JValue *sp = jv_get(root, "spells");
	for (int i = 0; i < jv_count(sp); i++) {
		char where[DIAG_WHERE_LEN];
		snprintf(where, sizeof(where), "spells[%d]", i);
		const JValue *e = jv_at(sp, i);
		check_obj(e, spec_spell, where, dl);
		if (jv_get(e, "id") == NULL)
			dl_add(dl, DIAG_ERROR, where, "needs an \"id\"");
		else {
			long long id = jv_get_int(e, "id", -1);
			if (id >= 0 && id < SPELL_IDS && id >= hero_spell_slots(f))
				dl_add(dl, DIAG_WARNING, where,
				    "spell %lld is not defined in %s", id, hero_flavor_name(f));
		}
	}

	/* Equipment: the seven named slots, each an item or null. */
	const JValue *eq = jv_get(root, "equipment");
	if (eq != NULL) {
		if (jv_type(eq) != JOBJ) {
			dl_add(dl, DIAG_ERROR, "equipment", "expected an object");
		} else {
			for (int i = 0; i < jv_count(eq); i++) {
				const char *key = jv_key_at(eq, i);
				int known = 0;
				for (int k = 0; k < NUM_INVLOC; k++)
					if (strcmp(charjson_equip_slots[k], key) == 0)
						known = 1;
				char where[DIAG_WHERE_LEN];
				join(where, sizeof(where), "equipment", key);
				if (!known) {
					const char *near = dl_nearest(key, charjson_equip_slots,
					    NUM_INVLOC);
					dl_add(dl, DIAG_ERROR, where,
					    near != NULL ? "unknown slot \"%s\" -- did you mean "
					                   "\"%s\"?"
					                 : "unknown equipment slot \"%s\"",
					    key, near);
					continue;
				}
				const JValue *it = jv_member_at(eq, i);
				if (jv_type(it) == JNULL)
					continue;
				check_obj(it, spec_item, where, dl);
				if (jv_get(it, "slot") != NULL)
					dl_add(dl, DIAG_WARNING, where,
					    "\"slot\" is ignored here; the equipment slot is the "
					    "key itself");
			}
		}
	}

	/* Inventory. */
	const JValue *inv = jv_get(root, "inventory");
	check_obj(inv, spec_inv, "inventory", dl);
	const JValue *items = jv_get(inv, "items");
	int slot_used[NUM_INV_GRID_ELEM];
	memset(slot_used, 0, sizeof(slot_used));
	for (int i = 0; i < jv_count(items); i++) {
		char where[DIAG_WHERE_LEN];
		snprintf(where, sizeof(where), "inventory.items[%d]", i);
		const JValue *e = jv_at(items, i);
		check_obj(e, spec_item, where, dl);
		long long slot = jv_get_int(e, "slot", i);
		if (slot >= 0 && slot < NUM_INV_GRID_ELEM) {
			if (slot_used[slot])
				dl_add(dl, DIAG_ERROR, where,
				    "slot %lld is claimed by an earlier entry too", slot);
			slot_used[slot] = 1;
		}
	}

	const JValue *grid = jv_get(inv, "grid");
	if (grid != NULL) {
		if (jv_count(grid) == 4 && jv_type(jv_at(grid, 0)) == JARR) {
			for (int row = 0; row < 4; row++) {
				char where[DIAG_WHERE_LEN];
				snprintf(where, sizeof(where), "inventory.grid[%d]", row);
				check_int_array(jv_at(grid, row), where, 10, R_CHAR, dl);
			}
		} else {
			check_int_array(grid, "inventory.grid", NUM_INV_GRID_ELEM, R_CHAR, dl);
		}
	}

	/* Belt. */
	const JValue *belt = jv_get(root, "belt");
	int belt_used[MAXBELTITEMS];
	memset(belt_used, 0, sizeof(belt_used));
	for (int i = 0; i < jv_count(belt); i++) {
		char where[DIAG_WHERE_LEN];
		snprintf(where, sizeof(where), "belt[%d]", i);
		const JValue *e = jv_at(belt, i);
		check_obj(e, spec_item, where, dl);
		long long slot = jv_get_int(e, "slot", i);
		if (slot < 0 || slot >= MAXBELTITEMS)
			dl_add(dl, DIAG_ERROR, where, "slot %lld is outside 0..%d", slot,
			    MAXBELTITEMS - 1);
		else {
			if (belt_used[slot])
				dl_add(dl, DIAG_ERROR, where,
				    "belt slot %lld is claimed twice", slot);
			belt_used[slot] = 1;
		}
	}

	check_int_array(jv_get(jv_get(root, "advanced"), "reserved_bytes"),
	    "advanced.reserved_bytes", 3, R_CHAR, dl);
	check_int_array(jv_get(jv_get(root, "advanced"), "reserved_dwords"),
	    "advanced.reserved_dwords", 5, R_INT, dl);

	jv_free(root);
}
