/**
 * @file json_check.cpp
 *
 * Phase 5 gate: the JSON interchange format.
 *
 * One property matters more than all the others: **export followed by import
 * must reproduce all 1266 bytes**. A user who exports a character, edits one
 * number, and imports has trusted the format with everything else. If a single
 * reserved byte is dropped the damage is silent -- and real Hellfire saves do
 * carry a nonzero bReserved[1], so this is not hypothetical.
 *
 * The strongest way to check that is to round-trip random bytes: if a struct
 * filled with noise survives, every field is genuinely represented rather than
 * merely the ones a hand-written fixture happened to set.
 */
#include "../src/charjson.h"

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

static void nowarn(const char *m)
{
	(void)m;
}

static int roundtrip(const PkPlayerStruct *in, PkPlayerStruct *out, HeroFlavor f)
{
	char err[JSON_ERR_LEN];
	char *text = charjson_write(in, f, nowarn);
	int r = charjson_read(text, out, NULL, err);
	if (!r)
		printf("        parse failed: %s\n", err);
	free(text);
	return r;
}

/* ------------------------------------------------------------------ */
/* 1. Lossless round-trip on random bytes                              */
/* ------------------------------------------------------------------ */

static uint32_t rng_state = 0x2A3B4C5Du;

static uint32_t rnd(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state >> 8;
}

/**
 * Fill a character with noise. One constraint, documented in charjson.h: the
 * name must be NUL-terminated with zero padding, which is the only form the
 * game ever writes.
 */
static void randomize(PkPlayerStruct *h)
{
	BYTE *p = (BYTE *)h;
	for (size_t i = 0; i < sizeof(*h); i++)
		p[i] = (BYTE)rnd();

	int len = 1 + (int)(rnd() % (PLR_NAME_LEN - 1));
	for (int i = 0; i < len; i++)
		h->pName[i] = (char)('a' + (rnd() % 26));
	for (int i = len; i < PLR_NAME_LEN; i++)
		h->pName[i] = '\0';
}

static void check_random_roundtrip(void)
{
	section("1. lossless round-trip on random bytes");

	int failures = 0;
	const int iterations = 400;
	char err[JSON_ERR_LEN];

	for (int it = 0; it < iterations; it++) {
		PkPlayerStruct a, b;
		randomize(&a);

		HeroFlavor f = (it % 2) ? FLAVOR_HELLFIRE : FLAVOR_DIABLO;
		char *text = charjson_write(&a, f, nowarn);

		HeroFlavor got_f = FLAVOR_DIABLO;
		if (!charjson_read(text, &b, &got_f, err)) {
			if (failures < 3)
				printf("        iteration %d failed to parse: %s\n", it, err);
			failures++;
			free(text);
			continue;
		}
		if (memcmp(&a, &b, sizeof(a)) != 0) {
			if (failures < 3) {
				const BYTE *x = (const BYTE *)&a, *y = (const BYTE *)&b;
				for (size_t i = 0; i < sizeof(a); i++)
					if (x[i] != y[i]) {
						printf("        iteration %d differs at byte %zu: "
						       "%02x -> %02x\n",
						    it, i, x[i], y[i]);
						break;
					}
			}
			failures++;
		}
		if (got_f != f)
			failures++;
		free(text);
	}

	okf(failures == 0, "%d random characters round-trip byte-for-byte", iterations);
}

/* ------------------------------------------------------------------ */
/* 2. The specific traps                                               */
/* ------------------------------------------------------------------ */

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

static void check_traps(void)
{
	section("2. the fields that are easy to lose");

	PkPlayerStruct h, back;

	/* Fractional life and mana. A real Monk is stored as 54 + 2/64. */
	base_hero(&h, PC_WARRIOR);
	h.pHPBase = 3458; /* 54 + 2/64 */
	h.pMaxHPBase = 3458;
	h.pManaBase = 2018; /* 31 + 34/64 */
	h.pMaxManaBase = 7020;
	ok(roundtrip(&h, &back, FLAVOR_DIABLO), "round-trip a character with fractions");
	ok(back.pHPBase == 3458 && back.pMaxHPBase == 3458,
	    "fractional life survives exactly (3458, not 3456)");
	ok(back.pManaBase == 2018 && back.pMaxManaBase == 7020,
	    "fractional mana survives exactly");

	/* The whole-point value is the readable one. */
	{
		char *text = charjson_write(&h, FLAVOR_DIABLO, nowarn);
		ok(strstr(text, "\"current\": 54") != NULL,
		    "life is written as whole points for editing");
		ok(strstr(text, "\"current_64ths\": 2") != NULL,
		    "the remainder is written beside it");
		free(text);
	}

	/* Zero fractions are omitted, keeping the common case clean. */
	base_hero(&h, PC_WARRIOR);
	{
		char *text = charjson_write(&h, FLAVOR_DIABLO, nowarn);
		ok(strstr(text, "_64ths") == NULL,
		    "no remainder keys when the fractions are zero");
		free(text);
	}

	/* Reserved bytes: real Hellfire saves have bReserved[1] == 1. */
	base_hero(&h, 3);
	h.bReserved[1] = 1;
	h.wReserved2 = -5;
	h.wReserved8 = 1234;
	h.dwReserved[3] = 999;
	h.destAction = -1;
	h.destParam1 = 5;
	h.destParam2 = 45;
	h.archiveTime.dwLowDateTime = 0xDEADBEEF;
	ok(roundtrip(&h, &back, FLAVOR_HELLFIRE), "round-trip reserved fields");
	ok(back.bReserved[1] == 1, "bReserved[1] survives");
	ok(back.wReserved2 == -5 && back.wReserved8 == 1234, "reserved shorts survive");
	ok(back.dwReserved[3] == 999, "reserved dwords survive");
	ok(back.destAction == -1 && back.destParam1 == 5 && back.destParam2 == 45,
	    "transient action state survives");
	ok(back.archiveTime.dwLowDateTime == 0xDEADBEEF, "archive time survives");

	/* pManaShield is a byte, not a bool; a value of 2 must not become 1. */
	base_hero(&h, PC_WARRIOR);
	h.pManaShield = 2;
	ok(roundtrip(&h, &back, FLAVOR_DIABLO) && back.pManaShield == 2,
	    "a non-boolean mana_shield byte survives");
	base_hero(&h, PC_WARRIOR);
	h.pManaShield = 1;
	{
		char *text = charjson_write(&h, FLAVOR_DIABLO, nowarn);
		ok(strstr(text, "\"mana_shield\": true") != NULL,
		    "but 0 and 1 still render as true/false");
		free(text);
	}

	/*
	 * Spell id 0 has a level byte but no spell-book bit: SPELLBIT(s) is
	 * 1 << (s-1), so touching a bit for id 0 would shift by -1.
	 */
	base_hero(&h, PC_WARRIOR);
	h.pSplLvl[0] = 7;
	ok(roundtrip(&h, &back, FLAVOR_DIABLO) && back.pSplLvl[0] == 7,
	    "spell id 0's level byte survives without a book bit");

	/* Hellfire spells 37..46 live in pSplLvl2. */
	base_hero(&h, 3);
	h.pSplLvl2[3] = 6; /* id 40 */
	h.pMemSpells |= SPELLBIT(40);
	ok(roundtrip(&h, &back, FLAVOR_HELLFIRE), "round-trip a pSplLvl2 spell");
	ok(back.pSplLvl2[3] == 6, "the level stayed in pSplLvl2");
	ok((back.pMemSpells & SPELLBIT(40)) != 0, "the book bit survives");
	ok(memcmp(back.pSplLvl, h.pSplLvl, MAX_SPELLS) == 0, "pSplLvl untouched");

	/* pMemSpells bits above the persistable range. */
	base_hero(&h, 3);
	h.pMemSpells = 0xF000000000000000ull;
	ok(roundtrip(&h, &back, FLAVOR_HELLFIRE) && back.pMemSpells == h.pMemSpells,
	    "spell-book bits outside ids 1..46 survive");

	/* Item fields: seed and buff are full 32-bit values. */
	base_hero(&h, PC_WARRIOR);
	h.InvBody[0].idx = 48;
	h.InvBody[0].iSeed = 0xFFFFFFFFu;
	h.InvBody[0].dwBuff = 0x80000001u;
	h.InvBody[0].iCreateInfo = 0xBEEF;
	h.InvBody[0].wValue = 0xCAFE;
	ok(roundtrip(&h, &back, FLAVOR_DIABLO), "round-trip an item");
	ok(back.InvBody[0].iSeed == 0xFFFFFFFFu, "a full-range item seed survives");
	ok(back.InvBody[0].dwBuff == 0x80000001u, "a high-bit dwBuff survives");
	ok(back.InvBody[0].iCreateInfo == 0xBEEF && back.InvBody[0].wValue == 0xCAFE,
	    "create_info and value survive");

	/* Grid values are signed: negative marks a continuation cell. */
	base_hero(&h, PC_WARRIOR);
	h.InvList[0].idx = 30;
	h._pNumInv = 1;
	h.InvGrid[0] = 1;
	h.InvGrid[10] = -1;
	ok(roundtrip(&h, &back, FLAVOR_DIABLO), "round-trip the inventory grid");
	ok(back.InvGrid[0] == 1 && back.InvGrid[10] == -1,
	    "negative grid entries survive");
}

/* ------------------------------------------------------------------ */
/* 3. Parser behaviour                                                 */
/* ------------------------------------------------------------------ */

static void check_parser(void)
{
	section("3. reader behaviour");

	PkPlayerStruct h;
	char err[JSON_ERR_LEN];

	/* A minimal document is legal; everything absent defaults to zero. */
	ok(charjson_read("{\"name\":\"Bob\",\"level\":5}", &h, NULL, err),
	    "a minimal document parses");
	ok(strcmp(h.pName, "Bob") == 0 && h.pLevel == 5, "the named fields are set");
	ok(h.pGold == 0 && h.pBaseStr == 0, "absent fields default to zero");
	ok(h.InvBody[0].idx == 0xFFFF && h.InvList[39].idx == 0xFFFF,
	    "absent item slots default to empty, not to gold");

	/* Class by name or by number, either flavor's names. */
	ok(charjson_read("{\"class\":\"Sorcerer\"}", &h, NULL, err) && h.pClass == 2,
	    "class by name");
	ok(charjson_read("{\"class\":\"barbarian\"}", &h, NULL, err) && h.pClass == 5,
	    "class by name is case-insensitive and knows Hellfire");
	ok(charjson_read("{\"class\":4}", &h, NULL, err) && h.pClass == 4,
	    "class by number");
	ok(!charjson_read("{\"class\":\"Necromancer\"}", &h, NULL, err),
	    "an unknown class name is rejected");
	printf("        %s\n", err);

	/* Comments and trailing commas, because these files get hand-edited. */
	ok(charjson_read("{\n // a line comment\n \"level\": 3, /* inline */\n}",
	       &h, NULL, err)
	        && h.pLevel == 3,
	    "comments and a trailing comma are accepted");

	/* Grid in either shape. */
	ok(charjson_read("{\"inventory\":{\"grid\":[[1,2,3,4,5,6,7,8,9,10],"
	                 "[0,0,0,0,0,0,0,0,0,0],[0,0,0,0,0,0,0,0,0,0],"
	                 "[0,0,0,0,0,0,0,0,0,11]]}}",
	       &h, NULL, err)
	        && h.InvGrid[0] == 1 && h.InvGrid[9] == 10 && h.InvGrid[39] == 11,
	    "grid as four rows of ten");
	ok(charjson_read("{\"inventory\":{\"grid\":[7,8,9]}}", &h, NULL, err)
	        && h.InvGrid[0] == 7 && h.InvGrid[2] == 9,
	    "grid as a flat array");

	/* Errors that should be clear rather than silent. */
	static const struct {
		const char *doc;
		const char *what;
	} bad[] = {
		{ "{", "unterminated object" },
		{ "[1,2,3]", "a top-level array" },
		{ "{\"level\": 3.5}", "a fractional number" },
		{ "{\"level\": }", "a missing value" },
		{ "{\"format\":\"something-else\"}", "a foreign format tag" },
		{ "{\"version\": 999}", "a future version" },
		{ "{\"game\":\"starcraft\"}", "an unknown game" },
		{ "{\"spells\":[{\"id\":99,\"level\":1}]}", "an unsaveable spell id" },
		{ "{\"inventory\":{\"items\":[{\"slot\":99,\"index\":1}]}}",
		    "an out-of-range inventory slot" },
		{ "{\"belt\":[{\"slot\":50,\"index\":1}]}", "an out-of-range belt slot" },
		{ "{\"name\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}",
		    "an over-long name" },
		{ "{} trailing junk", "trailing content" },
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		err[0] = '\0';
		okf(!charjson_read(bad[i].doc, &h, NULL, err), "%s is rejected", bad[i].what);
		if (err[0] != '\0')
			printf("        %s\n", err);
	}

	/* Parse errors should point at a place, not just complain. */
	err[0] = '\0';
	charjson_read("{\n  \"a\": 1,\n  \"b\": @\n}", &h, NULL, err);
	ok(strstr(err, "line 3") != NULL, "a syntax error reports its line");
	printf("        %s\n", err);
}

/* ------------------------------------------------------------------ */
/* 4. End to end through a save                                        */
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
		for (DWORD sc = 0; sc < nsec; sc++) {
			DWORD raw = file_len - sc * MPQ_SECTOR_SIZE;
			if (raw > MPQ_SECTOR_SIZE)
				raw = MPQ_SECTOR_SIZE;
			BYTE sector[MPQ_SECTOR_SIZE];
			memcpy(sector, entries[e].data + sc * MPQ_SECTOR_SIZE, raw);
			int stored = PkwareCompress(sector, (int)raw);
			offs[sc] = pos;
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
	section("4. save -> JSON -> edited JSON -> save");

	PkPlayerStruct h;
	base_hero(&h, 3); /* Monk */
	memset(h.pName, 0, PLR_NAME_LEN);
	strcpy(h.pName, "Jazreth");
	h.pHPBase = 3458; /* keep a fraction in play */
	h.pMaxHPBase = 3458;

	BYTE blob[1288];
	memset(blob, 0, sizeof(blob));
	memcpy(blob, &h, sizeof(h));
	codec_encode(blob, sizeof(h), 1288, SAVE_PASSWORD_SINGLE);
	Entry e = { "hero", blob, 1288 };
	const char *save = tmppath("single_0.hsv");
	ok(build_archive(save, &e, 1) == 0, "build a Hellfire save");

	char merr[MPQ_ERR_LEN];
	PkPlayerStruct from_save;
	ok(save_read_hero(save, &from_save, NULL, merr), "read it");

	/* Export, then rewrite one value in the text the way a person would. */
	char *text = charjson_write(&from_save, FLAVOR_HELLFIRE, nowarn);
	char *at = strstr(text, "\"vitality\": 35");
	ok(at != NULL, "found the vitality line in the exported text");
	if (at == NULL) {
		free(text);
		return;
	}
	/* "35" -> "77", same width so nothing else shifts. */
	memcpy(at + strlen("\"vitality\": "), "77", 2);

	PkPlayerStruct edited;
	char jerr[JSON_ERR_LEN];
	ok(charjson_read(text, &edited, NULL, jerr), "the edited text parses");
	free(text);

	ok(edited.pBaseVit == 77, "the edit took effect");
	ok(edited.pHPBase == 3458, "the fractional life was not disturbed");
	{
		PkPlayerStruct expect = from_save;
		expect.pBaseVit = 77;
		ok(memcmp(&edited, &expect, sizeof(expect)) == 0,
		    "and nothing else in the character changed");
	}

	ok(save_write_hero(save, &edited, merr), "write it back to the save");
	PkPlayerStruct back;
	ok(save_read_hero(save, &back, NULL, merr) && back.pBaseVit == 77
	        && back.pHPBase == 3458,
	    "the save round-trips the edit");

	/* The document is stable: exporting twice gives identical text. */
	char *t1 = charjson_write(&back, FLAVOR_HELLFIRE, nowarn);
	char *t2 = charjson_write(&back, FLAVOR_HELLFIRE, nowarn);
	ok(strcmp(t1, t2) == 0, "export is deterministic");
	ok(t1[strlen(t1) - 1] == '\n', "the document ends with a newline");
	free(t1);
	free(t2);
}

/* ------------------------------------------------------------------ */
/* 5. Real save (opt-in)                                               */
/* ------------------------------------------------------------------ */

static void check_real_save(void)
{
	section("5. real save");

	const char *path = getenv("BUTCHER_SAVE");
	if (path == NULL || path[0] == '\0') {
		printf("  SKIP  set BUTCHER_SAVE to round-trip a real character\n");
		return;
	}

	PkPlayerStruct h, back;
	char merr[MPQ_ERR_LEN];
	if (!save_read_hero(path, &h, NULL, merr)) {
		ok(0, "read the real save");
		printf("        %s\n", merr);
		return;
	}
	ok(1, "read the real save");

	HeroFlavor f = hero_flavor_for_path(path);
	ok(roundtrip(&h, &back, f), "export and re-import it");
	ok(memcmp(&h, &back, sizeof(h)) == 0,
	    "a REAL character round-trips through JSON byte-for-byte");

	if (memcmp(&h, &back, sizeof(h)) != 0) {
		const BYTE *x = (const BYTE *)&h, *y = (const BYTE *)&back;
		for (size_t i = 0; i < sizeof(h); i++)
			if (x[i] != y[i]) {
				printf("        first difference at byte %zu: %02x -> %02x\n",
				    i, x[i], y[i]);
				break;
			}
	}
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

	printf("Devilution character editor -- Phase 5 JSON format\n");
	printf("scratch dir: %s\n", g_tmpdir);

	check_random_roundtrip();
	check_traps();
	check_parser();
	check_end_to_end();
	check_real_save();

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
