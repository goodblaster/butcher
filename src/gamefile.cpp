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
	return 1;
}

/* ------------------------------------------------------------------ */
/* Applying                                                            */
/* ------------------------------------------------------------------ */

int game_apply(BYTE *buf, DWORD len, const GameLoc *loc, const PkPlayerStruct *hero)
{
	(void)len;
	BYTE *rec = buf + loc->name;
	int changed = 0;

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
