/**
 * @file gamefile.cpp
 *
 * See gamefile.h. The field table below is the whole of what this module
 * knows, and every entry in it was confirmed against real saves rather than
 * read off the game's source.
 */
#include "gamefile.h"

#include <stdarg.h>
#include <stddef.h> /* offsetof */

static void seterr(char *err, const char *fmt, ...)
{
	if (err == NULL)
		return;
	va_list va;
	va_start(va, fmt);
	vsnprintf(err, MPQ_ERR_LEN, fmt, va);
	va_end(va);
}

/* ------------------------------------------------------------------ */
/* Little-endian access                                                */
/* ------------------------------------------------------------------ */

/*
 * The player record is the original x86 struct image, so it is little-endian
 * regardless of what this runs on. (The fields *around* it, in LoadGame's own
 * prologue, are big-endian -- an inconsistency in the format, not a mistake
 * here. Nothing in the prologue is touched.)
 */
static int rd32(const BYTE *p)
{
	return (int)((DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16)
	    | ((DWORD)p[3] << 24));
}

static void wr32(BYTE *p, int v)
{
	DWORD u = (DWORD)v;
	p[0] = (BYTE)(u & 0xFF);
	p[1] = (BYTE)((u >> 8) & 0xFF);
	p[2] = (BYTE)((u >> 16) & 0xFF);
	p[3] = (BYTE)((u >> 24) & 0xFF);
}

static unsigned long long rd64(const BYTE *p)
{
	unsigned long long v = 0;
	for (int i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static void wr64(BYTE *p, unsigned long long v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (BYTE)((v >> (8 * i)) & 0xFF);
}

/* ------------------------------------------------------------------ */
/* The layout                                                          */
/* ------------------------------------------------------------------ */

/*
 * Offsets relative to _pName. Verified against two real saves -- a Hellfire
 * Rogue at level 25 and a Hellfire Monk at level 3 -- by reading each one and
 * comparing it to the same field in "hero". See docs/GAMEFILE.md for the
 * transcript.
 *
 * The prologue before the record is 43 bytes plus eight per dungeon level, and
 * _pName sits 320 into the record; neither is relied upon beyond seeding the
 * search, because both are exactly the kind of thing that shifts between
 * versions.
 */
#define GAME_PROLOGUE_FIXED 43
#define GAME_NAME_IN_RECORD 320

/** What kind of value lives at an offset. */
typedef enum FieldKind {
	F_I8,
	F_I32,
	F_U64,
	F_BYTES, /**< a run of bytes compared and copied whole */
	/**
	 * A NUL-terminated name in a fixed-width field.
	 *
	 * Compared only up to the terminator: the game does not clear the tail
	 * when a character is renamed, so a save can carry "rogue\0jh" where the
	 * packed copy holds "rogue\0\0\0". Comparing all 32 bytes would reject a
	 * perfectly healthy save. Writing does clear the tail.
	 */
	F_NAME
} FieldKind;

typedef struct Field {
	const char *name;
	long off;    /**< relative to _pName */
	FieldKind kind;
	size_t len;  /**< F_BYTES and F_NAME only */
	/** Where the same value lives in PkPlayerStruct. */
	size_t hero_off;
	size_t hero_len;
	/**
	 * Whether the packed field is signed.
	 *
	 * It matters: the stats are BYTE in PkPlayerStruct but int32 in the saved
	 * game, so a dexterity of 250 read as a signed char becomes -6 and gets
	 * written as -6. pClass and pLevel really are char.
	 */
	int is_signed;
} Field;

#define HF(m) offsetof(PkPlayerStruct, m), sizeof(((PkPlayerStruct *)0)->m)

static const Field kFields[] = {
	/*
	 * Spells first: one contiguous 47-byte run holding ids 0..46, which "hero"
	 * splits across pSplLvl[37] and pSplLvl2[10]. Confirmed contiguous on a
	 * Hellfire character carrying levels in both halves.
	 */
	{ "spell levels", -127, F_BYTES, 37, HF(pSplLvl), 1 },
	{ "Hellfire spell levels", -127 + 37, F_BYTES, 10, HF(pSplLvl2), 1 },
	{ "spell book", -56, F_U64, 0, HF(pMemSpells), 0 },

	{ "name", 0, F_NAME, PLR_NAME_LEN, HF(pName), 0 },
	{ "class", 32, F_I8, 0, HF(pClass), 1 },
	{ "strength", 40, F_I32, 0, HF(pBaseStr), 0 },
	{ "magic", 48, F_I32, 0, HF(pBaseMag), 0 },
	{ "dexterity", 56, F_I32, 0, HF(pBaseDex), 0 },
	{ "vitality", 64, F_I32, 0, HF(pBaseVit), 0 },
	{ "unspent points", 68, F_I32, 0, HF(pStatPts), 0 },
	{ "life", 80, F_I32, 0, HF(pHPBase), 1 },
	{ "max life", 84, F_I32, 0, HF(pMaxHPBase), 1 },
	{ "mana", 100, F_I32, 0, HF(pManaBase), 1 },
	{ "max mana", 104, F_I32, 0, HF(pMaxManaBase), 1 },
	{ "level", 120, F_I8, 0, HF(pLevel), 1 },
	{ "experience", 124, F_I32, 0, HF(pExperience), 1 },
	{ "gold", 140, F_I32, 0, HF(pGold), 1 },
};

static const int kNumFields = (int)(sizeof(kFields) / sizeof(kFields[0]));

/**
 * Two fields with no counterpart in the packed struct, so they cannot be
 * verified against it and are written from derived values instead.
 * Offsets confirmed on both real saves: _pNextExper read 8040 for the level-3
 * Monk and 5459523 for the level-25 Rogue, which are ExpLvlsTbl[3] and
 * ExpLvlsTbl[25] exactly.
 */
#define GAME_MAXLVL_OFF 121  /**< _pMaxLvl, the char after _pLevel */
#define GAME_NEXTEXP_OFF 132 /**< _pNextExper */

/** Lowest and highest byte the table touches, relative to _pName. */
#define GAME_SPAN_LO (-127)
#define GAME_SPAN_HI (144)

/** Read a field out of PkPlayerStruct as a signed value. */
static long long hero_value(const Field *f, const PkPlayerStruct *h)
{
	const BYTE *p = (const BYTE *)h + f->hero_off;
	switch (f->hero_len) {
	case 1:
		return f->is_signed ? (long long)(signed char)*p : (long long)*p;
	case 2:
		return (long long)(short)(p[0] | (p[1] << 8));
	case 8: {
		unsigned long long v;
		memcpy(&v, p, 8);
		return (long long)v;
	}
	default: {
		int v;
		memcpy(&v, p, 4);
		return v;
	}
	}
}

/** Read the same field out of the game blob. */
static long long game_value(const Field *f, const BYTE *rec)
{
	switch (f->kind) {
	case F_I8:
		return (long long)(signed char)rec[f->off];
	case F_U64:
		return (long long)rd64(rec + f->off);
	default:
		return rd32(rec + f->off);
	}
}

/* ------------------------------------------------------------------ */
/* Locating                                                            */
/* ------------------------------------------------------------------ */

/**
 * Compare every field at a candidate position.
 *
 * @param discriminating out; agreements where at least one side was non-zero.
 * @return nonzero if all fields agree.
 */
static int candidate_matches(const BYTE *buf, DWORD len, long name,
    const PkPlayerStruct *h, int *discriminating, const char **first_bad)
{
	*discriminating = 0;
	*first_bad = NULL;

	if (name + GAME_SPAN_LO < 0 || (DWORD)(name + GAME_SPAN_HI) > len)
		return 0;

	const BYTE *rec = buf + name;
	int ok = 1;

	for (int i = 0; i < kNumFields; i++) {
		const Field *f = &kFields[i];
		const BYTE *hp = (const BYTE *)h + f->hero_off;

		if (f->kind == F_NAME) {
			size_t n = strnlen((const char *)hp, f->len);
			if (n >= f->len || memcmp(rec + f->off, hp, n + 1) != 0) {
				if (*first_bad == NULL)
					*first_bad = f->name;
				ok = 0;
				continue;
			}
			if (n > 0)
				(*discriminating)++;
			continue;
		}

		if (f->kind == F_BYTES) {
			if (memcmp(rec + f->off, hp, f->len) != 0) {
				if (*first_bad == NULL)
					*first_bad = f->name;
				ok = 0;
				continue;
			}
			for (size_t b = 0; b < f->len; b++) {
				if (hp[b] != 0) {
					(*discriminating)++;
					break;
				}
			}
			continue;
		}

		long long g = game_value(f, rec);
		long long v = hero_value(f, h);
		if (g != v) {
			if (*first_bad == NULL)
				*first_bad = f->name;
			ok = 0;
			continue;
		}
		if (v != 0)
			(*discriminating)++;
	}
	return ok;
}

int game_locate(const BYTE *buf, DWORD len, const PkPlayerStruct *hero,
    GameLoc *loc, char *err)
{
	memset(loc, 0, sizeof(*loc));

	if (len < GAME_PROLOGUE_FIXED) {
		seterr(err, "the saved game is too short to hold a player");
		return 0;
	}

	/* The magic says which game wrote it, and so how many dungeon levels the
	 * prologue carries. Anything else is a format this does not know. */
	if (memcmp(buf, "RETL", 4) == 0) {
		loc->levels = 17;
	} else if (memcmp(buf, "HELF", 4) == 0) {
		loc->levels = 25;
		loc->hellfire = 1;
	} else if (memcmp(buf, "SHAR", 4) == 0) {
		loc->levels = 17;
	} else if (memcmp(buf, "SHLF", 4) == 0) {
		loc->levels = 25;
		loc->hellfire = 1;
	} else {
		seterr(err, "unrecognized saved-game format \"%.4s\"; expected RETL, "
		            "HELF, SHAR or SHLF",
		    (const char *)buf);
		return 0;
	}

	/*
	 * Scan for the name rather than computing where it should be. The expected
	 * position is checked first only so the common case is quick; a match
	 * anywhere is accepted provided it verifies, and two verifying matches are
	 * treated as a failure rather than a coin toss.
	 */
	long expected = GAME_PROLOGUE_FIXED + (long)loc->levels * 8 + GAME_NAME_IN_RECORD;

	long found = -1;
	int found_disc = 0;
	int hits = 0;
	const char *why = NULL;
	int near_miss = 0;

	size_t namelen = strnlen(hero->pName, PLR_NAME_LEN);
	if (namelen == 0 || namelen >= PLR_NAME_LEN) {
		seterr(err, "the character has no usable name to find it by");
		return 0;
	}

	for (long i = 0; i + PLR_NAME_LEN <= (long)len; i++) {
		if (memcmp(buf + i, hero->pName, namelen + 1) != 0)
			continue;

		int disc = 0;
		const char *bad = NULL;
		if (!candidate_matches(buf, len, i, hero, &disc, &bad)) {
			if (!near_miss) {
				why = bad;
				near_miss = 1;
			}
			continue;
		}
		if (disc < GAME_MIN_DISCRIMINATING)
			continue;

		hits++;
		if (found < 0 || i == expected) {
			found = i;
			found_disc = disc;
		}
	}

	if (hits == 0) {
		if (near_miss)
			seterr(err, "found the character's name in the saved game, but \"%s\" "
			            "there disagrees with the same field in \"hero\". This "
			            "build's save layout is not the one butcher was verified "
			            "against, so nothing was written.",
			    why != NULL ? why : "a field");
		else
			seterr(err, "could not find the character inside the saved game. The "
			            "saved-game layout is not one butcher recognizes, so "
			            "nothing was written.");
		return 0;
	}
	if (hits > 1) {
		seterr(err, "the character appears %d times in the saved game and there "
		            "is no way to tell which one the game loads; nothing was "
		            "written",
		    hits);
		return 0;
	}

	loc->name = found;
	loc->discriminating = found_disc;

	/*
	 * The inventory, anchored separately. InvGrid is 40 bytes that are
	 * byte-identical in both copies, and _pNumInv is the int32 right before it
	 * -- together specific enough to be trusted, and cheap to check.
	 *
	 * Not finding it is not a failure. The two copies diverge as soon as an
	 * earlier butcher release changed gold in the packed copy alone, and the
	 * stats above are still perfectly writable; only the inventory is refused.
	 */
	int grid_hits = 0;
	long grid_at = -1;
	for (long i = 4; i + NUM_INV_GRID_ELEM <= (long)len; i++) {
		if (memcmp(buf + i, hero->InvGrid, NUM_INV_GRID_ELEM) != 0)
			continue;
		if (rd32(buf + i - 4) != (int)hero->_pNumInv)
			continue;
		long list = i - 4 - (long)NUM_INV_GRID_ELEM * GAME_ITEM_SIZE;
		if (list < 0)
			continue;
		grid_hits++;
		grid_at = i;
	}
	if (grid_hits == 1) {
		loc->inv_found = 1;
		loc->inv_grid = grid_at;
		loc->inv_numinv = grid_at - 4;
		loc->inv_list = grid_at - 4 - (long)NUM_INV_GRID_ELEM * GAME_ITEM_SIZE;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Inventory                                                           */
/* ------------------------------------------------------------------ */

/** The item record for InvList[@p slot]. */
static BYTE *inv_slot(BYTE *buf, const GameLoc *loc, int slot)
{
	return buf + loc->inv_list + (long)slot * GAME_ITEM_SIZE;
}

/** Source/inv.cpp: the pile graphic follows the amount. */
static int gold_cursor(int value)
{
	if (value >= GOLD_MEDIUM_LIMIT)
		return ICURS_GOLD_LARGE;
	if (value <= GOLD_SMALL_LIMIT)
		return ICURS_GOLD_SMALL;
	return ICURS_GOLD_MEDIUM;
}

int game_apply_inventory(BYTE *buf, DWORD len, const GameLoc *loc,
    const PkPlayerStruct *old, const PkPlayerStruct *neu, char *err)
{
	if (!loc->inv_found) {
		seterr(err, "could not find the inventory inside the saved game; the two "
		            "copies of this character have already diverged, so the "
		            "items were left alone");
		return -1;
	}
	if (loc->inv_list < 0
	    || (DWORD)(loc->inv_grid + NUM_INV_GRID_ELEM) > len) {
		seterr(err, "the inventory lies outside the saved game");
		return -1;
	}
	if (old->_pNumInv > NUM_INV_GRID_ELEM || neu->_pNumInv > NUM_INV_GRID_ELEM) {
		seterr(err, "the item count is beyond the %d inventory slots",
		    NUM_INV_GRID_ELEM);
		return -1;
	}

	/*
	 * Every item the character had must be findable in the saved game by seed.
	 * That is the proof the two copies really do describe the same inventory,
	 * and it is checked before a byte is written.
	 */
	for (int i = 0; i < old->_pNumInv; i++) {
		const BYTE *rec = inv_slot(buf, loc, i);
		if ((DWORD)rd32(rec + GAME_ITEM_SEED) != old->InvList[i].iSeed) {
			seterr(err, "inventory slot %d disagrees between the two copies of "
			            "this character (seed %u against %u); the items were "
			            "left alone",
			    i, (unsigned)old->InvList[i].iSeed,
			    (unsigned)rd32(rec + GAME_ITEM_SEED));
			return -1;
		}
	}

	/* Snapshot, so items can be moved between slots without clobbering. */
	size_t span = (size_t)NUM_INV_GRID_ELEM * GAME_ITEM_SIZE;
	BYTE *before = (BYTE *)malloc(span);
	if (before == NULL) {
		seterr(err, "out of memory");
		return -1;
	}
	memcpy(before, buf + loc->inv_list, span);

	/*
	 * A gold item the game itself wrote, to clone new stacks from -- taken
	 * from the snapshot, not the live buffer. Pointing it at the buffer left
	 * it dangling the moment the loop below overwrote that slot, and every
	 * cloned pile came out with whatever had landed there instead.
	 */
	const BYTE *gold_template = NULL;
	for (int i = 0; i < old->_pNumInv; i++) {
		const BYTE *rec = before + (size_t)i * GAME_ITEM_SIZE;
		if (rd32(rec + GAME_ITEM_ITYPE) == ITYPE_GOLD) {
			gold_template = rec;
			break;
		}
	}

	int written = 0;
	for (int i = 0; i < neu->_pNumInv; i++) {
		const PkItemStruct *pk = &neu->InvList[i];
		const BYTE *src = NULL;

		/* The same item, wherever it used to sit. */
		for (int j = 0; j < old->_pNumInv; j++) {
			const BYTE *cand = before + (size_t)j * GAME_ITEM_SIZE;
			if ((DWORD)rd32(cand + GAME_ITEM_SEED) == pk->iSeed
			    && old->InvList[j].idx == pk->idx) {
				src = cand;
				break;
			}
		}

		if (src == NULL) {
			if (pk->idx != IDI_GOLD) {
				free(before);
				seterr(err, "inventory slot %d holds an item that is not in the "
				            "saved game and is not gold, so there is nothing to "
				            "build it from; the items were left alone",
				    i);
				return -1;
			}
			if (gold_template == NULL) {
				free(before);
				seterr(err, "adding gold needs an existing gold pile to copy, and "
				            "this character is carrying none. Pick up some gold "
				            "in game first, or edit gold with no game in progress");
				return -1;
			}
			src = gold_template;
		}

		BYTE *dst = inv_slot(buf, loc, i);
		if (src != dst)
			memmove(dst, src, GAME_ITEM_SIZE);

		if (pk->idx == IDI_GOLD) {
			/* Only the three things that make one pile differ from another. */
			wr32(dst + GAME_ITEM_SEED, (int)pk->iSeed);
			wr32(dst + GAME_ITEM_IVALUE, pk->wValue);
			wr32(dst + GAME_ITEM_ICURS, gold_cursor(pk->wValue));
		}
		written++;
	}

	free(before);

	wr32(buf + loc->inv_numinv, neu->_pNumInv);
	memcpy(buf + loc->inv_grid, neu->InvGrid, NUM_INV_GRID_ELEM);
	return written;
}

/* ------------------------------------------------------------------ */
/* Applying                                                            */
/* ------------------------------------------------------------------ */

int game_apply(BYTE *buf, DWORD len, const GameLoc *loc, const PkPlayerStruct *hero)
{
	(void)len;
	BYTE *rec = buf + loc->name;
	int changed = 0;

	/*
	 * Two fields the packed copy does not carry, so they are derived rather
	 * than copied.
	 *
	 * _pNextExper matters more than it looks. ValidatePlayer runs every tick
	 * and clamps _pExperience down to it, so leaving it stale would undo the
	 * experience being written here on the first frame of play.
	 */
	if (rd32(rec + GAME_NEXTEXP_OFF) != hero_next_exper(hero)) {
		wr32(rec + GAME_NEXTEXP_OFF, hero_next_exper(hero));
		changed++;
	}
	/* _pMaxLvl is the highest level reached; NextPlrLevel raises it in step. */
	if ((signed char)rec[GAME_MAXLVL_OFF] < hero->pLevel) {
		rec[GAME_MAXLVL_OFF] = (BYTE)hero->pLevel;
		changed++;
	}

	for (int i = 0; i < kNumFields; i++) {
		const Field *f = &kFields[i];
		const BYTE *hp = (const BYTE *)hero + f->hero_off;

		if (f->kind == F_BYTES || f->kind == F_NAME) {
			/* Written whole either way, which also clears any stale tail the
			 * game left behind from a previous name. */
			if (memcmp(rec + f->off, hp, f->len) != 0) {
				memcpy(rec + f->off, hp, f->len);
				changed++;
			}
			continue;
		}

		long long v = hero_value(f, hero);
		if (game_value(f, rec) == v)
			continue;

		switch (f->kind) {
		case F_I8:
			rec[f->off] = (BYTE)(signed char)v;
			break;
		case F_U64:
			wr64(rec + f->off, (unsigned long long)v);
			break;
		default:
			wr32(rec + f->off, (int)v);
			break;
		}
		changed++;
	}
	return changed;
}

void game_dump(const BYTE *buf, DWORD len, const GameLoc *loc,
    const PkPlayerStruct *hero, void *stream)
{
	FILE *out = (FILE *)stream;
	const BYTE *rec = buf + loc->name;

	fprintf(out, "saved game: %s, %d dungeon levels, %u bytes decoded\n",
	    loc->hellfire ? "Hellfire" : "Diablo", loc->levels, (unsigned)len);
	fprintf(out, "player record at %ld, name at %ld (%d fields agreed)\n\n",
	    loc->name - GAME_NAME_IN_RECORD, loc->name, loc->discriminating);
	fprintf(out, "  %-22s %-10s %14s %14s\n", "field", "offset", "saved game",
	    "packed copy");

	for (int i = 0; i < kNumFields; i++) {
		const Field *f = &kFields[i];
		const BYTE *hp = (const BYTE *)hero + f->hero_off;
		char off[16];
		snprintf(off, sizeof(off), "name%+ld", f->off);

		if (f->kind == F_NAME) {
			char a[PLR_NAME_LEN + 1], b[PLR_NAME_LEN + 1];
			snprintf(a, sizeof(a), "%.*s", (int)f->len, (const char *)rec + f->off);
			snprintf(b, sizeof(b), "%.*s", (int)f->len, (const char *)hp);
			fprintf(out, "  %-22s %-10s %14s %14s   %s\n", f->name, off, a, b,
			    strcmp(a, b) == 0 ? "" : "<-- DIFFER");
			continue;
		}
		if (f->kind == F_BYTES) {
			int same = memcmp(rec + f->off, hp, f->len) == 0;
			fprintf(out, "  %-22s %-10s %14s %14s   %s\n", f->name, off,
			    same ? "(same)" : "(differ)", "", same ? "" : "<-- DIFFER");
			continue;
		}
		long long g = game_value(f, rec);
		long long v = hero_value(f, hero);
		fprintf(out, "  %-22s %-10s %14lld %14lld   %s\n", f->name, off, g, v,
		    g == v ? "" : "<-- DIFFER");
	}
}

/* ------------------------------------------------------------------ */
/* Archive access                                                      */
/* ------------------------------------------------------------------ */

BYTE *game_read(const char *path, DWORD *out_len, int *present, char *err)
{
	if (present != NULL)
		*present = 0;

	MpqArchive *a = mpq_open(path, err);
	if (a == NULL)
		return NULL;
	if (!mpq_has_file(a, "game")) {
		mpq_close(a);
		return NULL; /* no game in progress: not an error */
	}

	/*
	 * Set as soon as the member exists, not once it has been read. Reporting
	 * "absent" for a game that is present but unreadable would send the caller
	 * down the hero-only path, which is precisely the silent half-edit this
	 * module exists to prevent.
	 */
	if (present != NULL)
		*present = 1;

	DWORD len = 0;
	BYTE *buf = mpq_read_file(a, "game", &len, err);
	mpq_close(a);
	if (buf == NULL)
		return NULL;

	DWORD dec = codec_decode(buf, len, SAVE_PASSWORD_SINGLE);
	if (dec == 0) {
		free(buf);
		seterr(err, "\"game\" did not decode; this may be a multiplayer save");
		return NULL;
	}
	*out_len = dec;
	return buf;
}

int game_write(const char *path, const BYTE *buf, DWORD len, char *err)
{
	DWORD enc = codec_get_encoded_len(len);
	BYTE *out = (BYTE *)malloc(enc);
	if (out == NULL) {
		seterr(err, "out of memory");
		return 0;
	}
	memcpy(out, buf, len);
	codec_encode(out, len, enc, SAVE_PASSWORD_SINGLE);

	int ok = mpq_replace_file(path, "game", out, enc, err);
	free(out);
	return ok;
}
