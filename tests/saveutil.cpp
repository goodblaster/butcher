/**
 * @file saveutil.cpp
 *
 * Gate for the shared save utilities -- discovery, backup, commit, and the
 * rename collision check.
 *
 * This code exists because both front ends need it and neither should shell
 * out to the other. It is therefore the layer where a mistake reaches the CLI
 * and the TUI at once, which is why the write path is checked hardest: a
 * commit must never leave a save it cannot read back, and must never quietly
 * destroy an existing backup.
 */
#include "../src/saveutil.h"

#include "../third_party/devilution/Source/encrypt.h"

#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/gamefile.h"

#include <sys/types.h>

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

static const char *g_tmpdir;

static const char *tmppath(const char *leaf)
{
	static char buf[1024];
	snprintf(buf, sizeof(buf), "%s/%s", g_tmpdir, leaf);
	return buf;
}

static uint64_t file_hash(const char *p)
{
	FILE *f = fopen(p, "rb");
	if (f == NULL)
		return 0;
	uint64_t h = 1469598103934665603ull;
	int c;
	while ((c = fgetc(f)) != EOF) {
		h ^= (uint8_t)c;
		h *= 1099511628211ull;
	}
	fclose(f);
	return h;
}

static int exists(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0;
}

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
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

/*
 * A minimal but genuine "game" member.
 *
 * Layout mirrors LoadGame: a 4-byte magic, its 39-byte prologue, eight bytes
 * per dungeon level, then the player record with _pName 320 in. Only the
 * fields gamefile.cpp knows about are filled; the rest stays zero, which is
 * what a real save's padding and pointer slots look like anyway.
 */
#define GAME_FIXTURE_LEVELS 25
#define GAME_FIXTURE_RECORD (43 + GAME_FIXTURE_LEVELS * 8)
#define GAME_FIXTURE_NAME (GAME_FIXTURE_RECORD + 320)
/*
 * The inventory too, or the part of gamefile.cpp that writes items would never
 * be exercised. Its position does not have to match a real save -- the anchor
 * is searched for -- but InvList really is 40 full ItemStructs, so the blob is
 * some 15 KB and spans several MPQ sectors.
 */
#define GAME_FIXTURE_INVLIST (GAME_FIXTURE_NAME + 600)
#define GAME_FIXTURE_NUMINV (GAME_FIXTURE_INVLIST + NUM_INV_GRID_ELEM * GAME_ITEM_SIZE)
#define GAME_FIXTURE_GRID (GAME_FIXTURE_NUMINV + 4)
#define GAME_FIXTURE_LEN (GAME_FIXTURE_GRID + NUM_INV_GRID_ELEM + 64)

static void put32(BYTE *p, int v)
{
	p[0] = (BYTE)v;
	p[1] = (BYTE)(v >> 8);
	p[2] = (BYTE)(v >> 16);
	p[3] = (BYTE)(v >> 24);
}

/**
 * @param stale_tail leave junk after the name, as a renamed character has.
 * @param levels     17 for a Diablo saved game, 25 for Hellfire. Changing it
 *                   moves the whole record, which is the point: both anchors
 *                   are searched for, so neither should care.
 */
static void make_game_blob_ex(BYTE *out, const PkPlayerStruct *h, int stale_tail,
    int levels)
{
	long name_at = 43 + (long)levels * 8 + 320;
	long list_at = name_at + 600;
	long numinv_at = list_at + (long)NUM_INV_GRID_ELEM * GAME_ITEM_SIZE;
	long grid_at = numinv_at + 4;

	memset(out, 0, GAME_FIXTURE_LEN);
	memcpy(out, levels == 25 ? "HELF" : "RETL", 4);

	BYTE *rec = out + name_at;
	memcpy(rec - 127, h->pSplLvl, 37);
	memcpy(rec - 127 + 37, h->pSplLvl2, 10);
	memcpy(rec - 56, &h->pMemSpells, 8);

	memcpy(rec, h->pName, PLR_NAME_LEN);
	if (stale_tail) {
		size_t n = strnlen(h->pName, PLR_NAME_LEN);
		if (n + 2 < PLR_NAME_LEN) {
			rec[n + 1] = 'j';
			rec[n + 2] = 'h';
		}
	}
	rec[32] = (BYTE)h->pClass;
	put32(rec + 40, h->pBaseStr);
	put32(rec + 48, h->pBaseMag);
	put32(rec + 56, h->pBaseDex);
	put32(rec + 64, h->pBaseVit);
	put32(rec + 68, h->pStatPts);
	put32(rec + 80, h->pHPBase);
	put32(rec + 84, h->pMaxHPBase);
	put32(rec + 100, h->pManaBase);
	put32(rec + 104, h->pMaxManaBase);
	rec[120] = (BYTE)h->pLevel;
	put32(rec + 124, h->pExperience);
	put32(rec + 140, h->pGold);

	for (int i = 0; i < h->_pNumInv; i++) {
		BYTE *it = out + list_at + (size_t)i * GAME_ITEM_SIZE;
		put32(it + GAME_ITEM_SEED, (int)h->InvList[i].iSeed);
		put32(it + GAME_ITEM_ITYPE,
		    h->InvList[i].idx == IDI_GOLD ? ITYPE_GOLD : ITYPE_MISC);
		put32(it + GAME_ITEM_IVALUE,
		    h->InvList[i].idx == IDI_GOLD ? h->InvList[i].wValue : 0);
	}
	put32(out + numinv_at, h->_pNumInv);
	memcpy(out + grid_at, h->InvGrid, NUM_INV_GRID_ELEM);
}

/** @param stale_tail leave junk after the name, as a renamed character has. */
static void make_game_blob(BYTE *out, const PkPlayerStruct *h, int stale_tail)
{
	make_game_blob_ex(out, h, stale_tail, GAME_FIXTURE_LEVELS);
}

/**
 * Write a one-file save archive containing this character.
 * @param with_game 0 none, 1 a Hellfire saved game, 2 one with a stale name
 *                  tail, 3 a Diablo (RETL) one.
 */
static int make_save_ex(const char *path, const PkPlayerStruct *hero, int with_game)
{
	BYTE blob[1288];
	memset(blob, 0, sizeof(blob));
	memcpy(blob, hero, sizeof(*hero));
	codec_encode(blob, sizeof(*hero), 1288, SAVE_PASSWORD_SINGLE);

	_FILEHEADER hdr;
	_BLOCKENTRY *block = (_BLOCKENTRY *)calloc(MPQ_INDEX_ENTRIES, sizeof(_BLOCKENTRY));
	_HASHENTRY *hash = (_HASHENTRY *)malloc(MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
	memset(hash, 0xFF, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
	InitHash();

	static BYTE body[131072];
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

	DWORD total = offs[1];
	if (with_game) {
		/*
		 * A real saved game, not a placeholder: magic, LoadGame's prologue, and
		 * a player record carrying the same character. Anything less would let
		 * the commit path skip the part under test.
		 */
		BYTE plain[GAME_FIXTURE_LEN];
		make_game_blob_ex(plain, hero, with_game == 2,
		    with_game == 3 ? 17 : GAME_FIXTURE_LEVELS);
		DWORD genc = codec_get_encoded_len(sizeof(plain));
		BYTE *gbuf = (BYTE *)malloc(genc);
		memcpy(gbuf, plain, sizeof(plain));
		codec_encode(gbuf, sizeof(plain), genc, SAVE_PASSWORD_SINGLE);

		/* Stored the way the game stores it: imploded, in 4096-byte sectors. */
		DWORD nsec = (genc + MPQ_SECTOR_SIZE - 1) / MPQ_SECTOR_SIZE;
		DWORD gstart = total;
		DWORD *goffs = (DWORD *)(body + gstart);
		DWORD pos = (nsec + 1) * (DWORD)sizeof(DWORD);

		for (DWORD s = 0; s < nsec; s++) {
			DWORD raw = genc - s * MPQ_SECTOR_SIZE;
			if (raw > MPQ_SECTOR_SIZE)
				raw = MPQ_SECTOR_SIZE;
			BYTE gsector[MPQ_SECTOR_SIZE];
			memcpy(gsector, gbuf + s * MPQ_SECTOR_SIZE, raw);
			int gstored = PkwareCompress(gsector, (int)raw);
			goffs[s] = pos;
			memcpy(body + gstart + pos, gsector, (size_t)gstored);
			pos += (DWORD)gstored;
		}
		goffs[nsec] = pos;
		free(gbuf);

		block[1].offset = (int)(MPQ_DATA_OFFSET + gstart);
		block[1].sizealloc = (int)pos;
		block[1].sizefile = (int)genc;
		block[1].flags = (int)(MPQ_FLAG_EXISTS | MPQ_FLAG_IMPLODE);
		DWORD gi = Hash("game", 0) & 0x7FF;
		while (hash[gi].block != -1)
			gi = (gi + 1) & 0x7FF;
		hash[gi].hashcheck[0] = (int)Hash("game", 1);
		hash[gi].hashcheck[1] = (int)Hash("game", 2);
		hash[gi].lcid = 0;
		hash[gi].block = 1;
		total = gstart + pos;
	}

	memset(&hdr, 0, sizeof(hdr));
	hdr.signature = (int)MPQ_SIGNATURE;
	hdr.headersize = 32;
	hdr.filesize = (int)(MPQ_DATA_OFFSET + total);
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
		fwrite(body, total, 1, f);
		fclose(f);
		chmod(path, 0644);
		rc = 0;
	}
	free(block);
	free(hash);
	return rc;
}

static int make_save(const char *path, const PkPlayerStruct *hero)
{
	return make_save_ex(path, hero, /*with_game=*/0);
}

/* ------------------------------------------------------------------ */
/* 0. Games in progress                                                */
/* ------------------------------------------------------------------ */

/*
 * From a real report: a Rogue's dexterity was edited, showed correctly on the
 * character-selection screen, and was back to its old value once the game
 * started. The character is stored twice. "hero" is the packed struct this
 * tool edits and the selection screen displays; "game" holds a full
 * PlayerStruct, and choosing that character runs LoadGame -> LoadPlayer, which
 * overwrites the player from it (DevilutionX Source/loadsave.cpp).
 *
 * butcher cannot edit the "game" copy, so the least it can do is say so. An
 * edit that is visible but not effective is the worst way for this to fail.
 */
static void check_game_in_progress(void)
{
	section("0. saves holding a game in progress");

	PkPlayerStruct h;
	base_hero(&h, "Aidan", PC_WARRIOR);

	/* tmppath returns a static buffer, so each path needs its own copy. */
	char plain[SAVE_PATH_MAX], playing[SAVE_PATH_MAX];
	snprintf(plain, sizeof(plain), "%s", tmppath("single_3.sv"));
	snprintf(playing, sizeof(playing), "%s", tmppath("single_4.sv"));
	make_save_ex(plain, &h, /*with_game=*/0);
	base_hero(&h, "Moreina", PC_ROGUE);
	make_save_ex(playing, &h, /*with_game=*/1);

	ok(!save_has_game(plain), "a save with no game in progress is not flagged");
	ok(save_has_game(playing), "one that holds a game is");

	/* It has to survive a write, or the flag would vanish on the first edit. */
	char err[MPQ_ERR_LEN];
	PkPlayerStruct edited;
	ok(save_read_hero(playing, &edited, NULL, err), "the flagged save still reads");
	edited.pBaseDex = 250;
	err[0] = '\0';
	if (!save_commit(playing, &edited, /*backup=*/0, err))
		printf("        %s\n", err);
	ok(err[0] == '\0', "and still writes");
	ok(save_has_game(playing),
	    "the game file survives a rewrite, so the warning does not disappear");

	PkPlayerStruct back;
	ok(save_read_hero(playing, &back, NULL, err) && back.pBaseDex == 250,
	    "  ...and the edit reached \"hero\"");

	/* The point of all this: the saved game has to have moved too. */
	{
		DWORD glen = 0;
		int present = 0;
		BYTE *g = game_read(playing, &glen, &present, err);
		ok(present && g != NULL, "the saved game still reads back");
		if (g != NULL) {
			GameLoc loc;
			ok(game_locate(g, glen, &back, &loc, err),
			    "  ...and still verifies against the edited hero");
			free(g);
		}
	}

	/*
	 * 250 is the case that caught a real bug: the stats are BYTE in the packed
	 * struct but int32 in the saved game, so reading them as signed turned 250
	 * into -6 and wrote -6.
	 */
	{
		DWORD glen = 0;
		int present = 0;
		BYTE *g = game_read(playing, &glen, &present, err);
		GameLoc loc;
		if (g != NULL && game_locate(g, glen, &back, &loc, err)) {
			const BYTE *rec = g + loc.name;
			int dex = (int)((DWORD)rec[56] | ((DWORD)rec[57] << 8)
			    | ((DWORD)rec[58] << 16) | ((DWORD)rec[59] << 24));
			if (dex != 250)
				printf("        dexterity came back as %d\n", dex);
			ok(dex == 250, "a stat above 127 survives unsigned, not as a negative");
		} else {
			ok(0, "a stat above 127 survives unsigned");
		}
		free(g);
	}

	/* A present-but-unreadable saved game must fail the commit, not be skipped:
	 * writing only "hero" is the silent half-edit this exists to prevent. */
	{
		char broken[SAVE_PATH_MAX];
		snprintf(broken, sizeof(broken), "%s", tmppath("single_5.sv"));
		make_save_ex(broken, &back, /*with_game=*/1);
		/* Corrupt the archive's game member by truncating the file. */
		FILE *f = fopen(broken, "rb+");
		if (f != NULL) {
			fseek(f, 0, SEEK_END);
			long sz = ftell(f);
			fclose(f);
			truncate(broken, sz - 64);
		}
		err[0] = '\0';
		PkPlayerStruct edit2 = back;
		edit2.pBaseStr = 44;
		int wrote = save_commit(broken, &edit2, /*backup=*/0, err);
		ok(!wrote && err[0] != '\0',
		    "a damaged saved game fails the commit rather than being skipped");
		printf("        %s\n", err);

		PkPlayerStruct after2;
		if (save_read_hero(broken, &after2, NULL, err))
			ok(after2.pBaseStr != 44, "  ...and \"hero\" was left alone");
		else
			ok(1, "  ...and \"hero\" was left alone (unreadable, never written)");
		remove(broken);
	}

	/* A stats-only edit does not touch the inventory. */
	{
		SaveGameSync sync = SAVE_GAME_ABSENT;
		PkPlayerStruct stats = back;
		stats.pBaseMag = 60;
		ok(save_commit_ex(playing, &stats, 0, &sync, err)
		        && sync == SAVE_GAME_SYNCED,
		    "a stats-only edit reports a clean sync");
	}

	/*
	 * Gold, which is the whole reason the inventory has to be written.
	 * ValidatePlayer recomputes the total from the stacks every tick, so a
	 * change that reaches only the packed copy is undone on the first frame.
	 */
	{
		char gpath[SAVE_PATH_MAX];
		snprintf(gpath, sizeof(gpath), "%s", tmppath("single_7.sv"));
		base_hero(&h, "Aidan", PC_WARRIOR);
		ok(hero_set_gold(&h, FLAVOR_DIABLO, 3000, err), "a character with one pile");
		make_save_ex(gpath, &h, /*with_game=*/1);

		PkPlayerStruct rich = h;
		ok(hero_set_gold(&rich, FLAVOR_DIABLO, 40000, err), "raise it to 40000");

		SaveGameSync sync = SAVE_GAME_ABSENT;
		err[0] = '\0';
		int wrote = save_commit_ex(gpath, &rich, 0, &sync, err);
		if (!wrote)
			printf("        %s\n", err);
		ok(wrote && sync == SAVE_GAME_SYNCED_ITEMS,
		    "the commit reports the inventory went across too");

		/* The stacks inside the saved game must now add up. */
		DWORD glen = 0;
		int present = 0;
		BYTE *g = game_read(gpath, &glen, &present, err);
		GameLoc loc;
		PkPlayerStruct on_disk;
		ok(g != NULL && save_read_hero(gpath, &on_disk, NULL, err)
		        && game_locate(g, glen, &on_disk, &loc, err),
		    "the saved game still verifies against the packed copy");
		ok(loc.inv_found, "  ...and its inventory is still locatable");

		if (g != NULL && loc.inv_found) {
			int total = 0, stacks = 0, seeds_agree = 1;
			for (int i = 0; i < on_disk._pNumInv; i++) {
				const BYTE *rec = g + loc.inv_list + (long)i * GAME_ITEM_SIZE;
				int type, val, seed;
				memcpy(&type, rec + GAME_ITEM_ITYPE, 4);
				memcpy(&val, rec + GAME_ITEM_IVALUE, 4);
				memcpy(&seed, rec + GAME_ITEM_SEED, 4);
				if (type != ITYPE_GOLD)
					continue;
				stacks++;
				total += val;
				/* The seed ties each written stack to its packed counterpart. */
				if ((DWORD)seed != on_disk.InvList[i].iSeed)
					seeds_agree = 0;
			}
			ok(total == 40000,
			    "the gold stacks inside the saved game add up to the new total");
			ok(stacks == 8, "  ...spread over eight piles of 5000");
			ok(seeds_agree, "  ...each carrying the packed copy's seed");

			int num = 0;
			memcpy(&num, g + loc.inv_numinv, 4);
			ok(num == on_disk._pNumInv, "_pNumInv agrees");
			ok(memcmp(g + loc.inv_grid, on_disk.InvGrid, NUM_INV_GRID_ELEM) == 0,
			    "and InvGrid was carried across");
		}
		free(g);

		/* Adding gold needs a pile to clone; refuse rather than invent one. */
		PkPlayerStruct broke = rich;
		ok(hero_set_gold(&broke, FLAVOR_DIABLO, 0, err), "spend it all");
		ok(save_commit_ex(gpath, &broke, 0, NULL, err), "which writes fine");

		PkPlayerStruct again;
		ok(save_read_hero(gpath, &again, NULL, err), "read it back");
		ok(hero_set_gold(&again, FLAVOR_DIABLO, 5000, err), "try to add gold again");
		err[0] = '\0';
		ok(!save_commit_ex(gpath, &again, 0, NULL, err),
		    "adding gold with no pile to copy is refused");
		printf("        %s\n", err);

		PkPlayerStruct after;
		ok(save_read_hero(gpath, &after, NULL, err) && after.pGold == 0,
		    "  ...and the save is left as it was");
		remove(gpath);
	}

	/*
	 * A Diablo saved game. RETL has a shorter prologue than HELF -- 17 dungeon
	 * levels rather than 25 -- which moves the whole record and everything
	 * after it. Neither anchor is computed from that, both are searched for, so
	 * nothing should notice; this is what says so out loud.
	 *
	 * Worth stating plainly: this exercises the code path, not a real Diablo
	 * saved game, which I have none of. The record itself is the same
	 * PlayerStruct in both games, and a layout that had shifted would fail
	 * verification rather than corrupt anything.
	 */
	{
		char dpath[SAVE_PATH_MAX];
		snprintf(dpath, sizeof(dpath), "%s", tmppath("single_9.sv"));
		base_hero(&h, "Aidan", PC_WARRIOR);
		ok(hero_set_gold(&h, FLAVOR_DIABLO, 2000, err), "a Diablo character");
		make_save_ex(dpath, &h, /*with_game=*/3);

		DWORD glen = 0;
		int present = 0;
		BYTE *g = game_read(dpath, &glen, &present, err);
		GameLoc loc;
		ok(g != NULL && game_locate(g, glen, &h, &loc, err),
		    "a RETL saved game is located just like a HELF one");
		if (g != NULL) {
			ok(!loc.hellfire && loc.levels == 17, "  ...and reports 17 levels");
			ok(loc.inv_found, "  ...with its inventory found too");
			/* The shifted prologue must not have shifted the anchors. */
			ok(loc.name == 43 + 17 * 8 + 320,
			    "  ...at the offset the shorter prologue implies");
		}
		free(g);

		/* And a full edit round-trips through it. */
		PkPlayerStruct rich = h;
		ok(hero_set_gold(&rich, FLAVOR_DIABLO, 25000, err), "raise its gold");
		rich.pBaseStr = 99;
		SaveGameSync sync = SAVE_GAME_ABSENT;
		err[0] = '\0';
		int wrote = save_commit_ex(dpath, &rich, 0, &sync, err);
		if (!wrote)
			printf("        %s\n", err);
		ok(wrote && sync == SAVE_GAME_SYNCED_ITEMS,
		    "  ...and an edit writes through to it, inventory included");

		PkPlayerStruct back2;
		ok(save_read_hero(dpath, &back2, NULL, err) && back2.pGold == 25000
		        && back2.pBaseStr == 99,
		    "  ...landing on disk");
		remove(dpath);
	}

	/* A renamed character leaves junk after the terminator in "game" but not in
	 * "hero"; comparing the whole field would reject a healthy save. */
	{
		char tail[SAVE_PATH_MAX];
		snprintf(tail, sizeof(tail), "%s", tmppath("single_6.sv"));
		base_hero(&h, "Aidan", PC_WARRIOR);
		make_save_ex(tail, &h, /*with_game=*/2);
		DWORD glen = 0;
		int present = 0;
		BYTE *g = game_read(tail, &glen, &present, err);
		GameLoc loc;
		ok(g != NULL && game_locate(g, glen, &h, &loc, err),
		    "a stale name tail in the saved game is not treated as a mismatch");
		free(g);
		remove(tail);
	}

	/* Discovery carries the flag through, so both front ends can show it. */
	SaveEntry found[SAVE_MAX_SLOTS];
	char dir[SAVE_PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", g_tmpdir);
	int n = save_scan_dir(dir, found, SAVE_MAX_SLOTS);
	int saw_plain = 0, saw_playing = 0;
	for (int i = 0; i < n; i++) {
		if (strcmp(found[i].name, "Aidan") == 0 && !found[i].in_progress)
			saw_plain = 1;
		if (strcmp(found[i].name, "Moreina") == 0 && found[i].in_progress)
			saw_playing = 1;
	}
	ok(saw_plain && saw_playing, "scanning a directory reports it per save");

	remove(plain);
	remove(playing);
}

/* ------------------------------------------------------------------ */
/* 1. Discovery                                                        */
/* ------------------------------------------------------------------ */

static void check_discovery(void)
{
	section("1. finding saves");

	char dir[SAVE_PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/saves", g_tmpdir);
	mkdir(dir, 0755);

	PkPlayerStruct a, b, c;
	base_hero(&a, "Aidan", PC_WARRIOR);
	base_hero(&b, "Moreina", PC_ROGUE);
	base_hero(&c, "Jazreth", 3); /* Monk -- Hellfire */
	char p[SAVE_PATH_MAX];

	snprintf(p, sizeof(p), "%s/single_0.sv", dir);
	make_save(p, &a);
	snprintf(p, sizeof(p), "%s/single_3.sv", dir);
	make_save(p, &b);
	snprintf(p, sizeof(p), "%s/single_0.hsv", dir);
	make_save(p, &c);

	SaveEntry found[SAVE_MAX_SLOTS];
	int n = save_scan_dir(dir, found, SAVE_MAX_SLOTS);
	ok(n == 3, "finds all three saves");

	int diablo = 0, hellfire = 0;
	for (int i = 0; i < n; i++)
		(found[i].flavor == FLAVOR_HELLFIRE ? hellfire : diablo)++;
	ok(diablo == 2 && hellfire == 1, "flavors come from the extension");

	int slots_ok = 0;
	for (int i = 0; i < n; i++)
		if ((strcmp(found[i].name, "Moreina") == 0 && found[i].slot == 3)
		    || (strcmp(found[i].name, "Aidan") == 0 && found[i].slot == 0)
		    || (strcmp(found[i].name, "Jazreth") == 0 && found[i].slot == 0))
			slots_ok++;
	ok(slots_ok == 3, "names and slot numbers are read correctly");

	/* Non-save files in the directory must be ignored, not misread. */
	snprintf(p, sizeof(p), "%s/notes.txt", dir);
	FILE *junk = fopen(p, "w");
	fputs("hello", junk);
	fclose(junk);
	snprintf(p, sizeof(p), "%s/single_1.sv", dir);
	junk = fopen(p, "wb");
	fputs("not an archive", junk);
	fclose(junk);
	ok(save_scan_dir(dir, found, SAVE_MAX_SLOTS) == 3,
	    "unreadable and unrelated files are skipped, not counted");

	char empty[SAVE_PATH_MAX];
	snprintf(empty, sizeof(empty), "%s/nothing-here", g_tmpdir);
	mkdir(empty, 0755);
	ok(save_scan_dir(empty, found, SAVE_MAX_SLOTS) == 0, "an empty directory finds none");
	ok(save_scan_dir("/no/such/place", found, SAVE_MAX_SLOTS) == 0,
	    "a missing directory finds none rather than failing");

	/* The platform defaults must at least be well-formed paths. */
	char dirs[8][SAVE_PATH_MAX];
	int nd = save_default_dirs(dirs, 8);
	ok(nd >= 0 && nd <= 8, "default locations enumerate without error");
	for (int i = 0; i < nd; i++)
		ok(dirs[i][0] == '/', "  ...and are absolute");
}

/* ------------------------------------------------------------------ */
/* 2. Backups                                                          */
/* ------------------------------------------------------------------ */

static void check_backup(void)
{
	section("2. backups");

	PkPlayerStruct h;
	base_hero(&h, "Aidan", PC_WARRIOR);
	const char *save = tmppath("bk.sv");
	make_save(save, &h);

	char bak[SAVE_PATH_MAX];
	snprintf(bak, sizeof(bak), "%s.bak", save);
	remove(bak);
	for (int i = 1; i <= 3; i++) {
		char n[SAVE_PATH_MAX];
		snprintf(n, sizeof(n), "%s.bak.%d", save, i);
		remove(n);
	}

	uint64_t before = file_hash(save);
	char err[MPQ_ERR_LEN] = { 0 };

	char chosen[SAVE_PATH_MAX] = { 0 };
	ok(save_backup_to(save, chosen, sizeof(chosen), err), "first backup is written");
	ok(exists(bak), "the .bak file exists");
	ok(file_hash(bak) == before, "and is a faithful copy");
	ok(strcmp(chosen, bak) == 0, "and its name is reported back");

	/*
	 * A second backup must not overwrite the first: that one is the pristine
	 * original, and replacing it with an already-edited copy destroys the very
	 * thing being kept. But refusing outright cost the user their edit, so it
	 * takes the next free name instead.
	 */
	base_hero(&h, "Edited", PC_WARRIOR);
	make_save(save, &h);
	uint64_t edited = file_hash(save);
	ok(edited != before, "the save has since changed");

	err[0] = '\0';
	chosen[0] = '\0';
	ok(save_backup_to(save, chosen, sizeof(chosen), err),
	    "a second backup succeeds rather than failing the save");
	ok(file_hash(bak) == before, "the original .bak is untouched");

	char bak1[SAVE_PATH_MAX];
	snprintf(bak1, sizeof(bak1), "%s.bak.1", save);
	ok(strcmp(chosen, bak1) == 0, "it fell back to .bak.1");
	ok(file_hash(bak1) == edited, "which holds the current contents");

	/* And keeps counting. */
	chosen[0] = '\0';
	ok(save_backup_to(save, chosen, sizeof(chosen), err), "a third backup too");
	char bak2[SAVE_PATH_MAX];
	snprintf(bak2, sizeof(bak2), "%s.bak.2", save);
	ok(strcmp(chosen, bak2) == 0, "landing on .bak.2");
	ok(file_hash(bak) == before && file_hash(bak1) == edited,
	    "with every earlier backup still intact");

	/* The plain wrapper still works and still never overwrites. */
	ok(save_backup(save, err), "save_backup picks a free name without reporting it");
	ok(file_hash(bak) == before, "and leaves .bak alone");
}

/* ------------------------------------------------------------------ */
/* 3. Committing                                                       */
/* ------------------------------------------------------------------ */

static void check_commit(void)
{
	section("3. committing a change");

	PkPlayerStruct h;
	base_hero(&h, "Aidan", PC_WARRIOR);
	const char *save = tmppath("commit.sv");
	make_save(save, &h);

	char err[MPQ_ERR_LEN] = { 0 };
	char bak[SAVE_PATH_MAX], bak1[SAVE_PATH_MAX];
	snprintf(bak, sizeof(bak), "%s.bak", save);
	snprintf(bak1, sizeof(bak1), "%s.bak.1", save);
	remove(bak);
	remove(bak1);

	PkPlayerStruct edited = h;
	edited.pBaseStr = 99;
	edited.pGold = 0;

	ok(save_commit(save, &edited, /*backup=*/1, err), "commit with a backup");
	ok(exists(bak), "the backup was written");
	uint64_t pristine = file_hash(bak);

	PkPlayerStruct back;
	ok(save_read_hero(save, &back, NULL, err), "the save reads back");
	ok(back.pBaseStr == 99, "the edit landed");
	ok(memcmp(&back, &edited, sizeof(back)) == 0, "the whole character matches");

	/*
	 * A second commit must not clobber the first backup -- but it must still
	 * go through. Failing the write to protect the backup lost the edit, which
	 * is the worse of the two.
	 */
	edited.pBaseStr = 100;
	err[0] = '\0';
	ok(save_commit(save, &edited, /*backup=*/1, err),
	    "a second backed-up commit succeeds");
	ok(file_hash(bak) == pristine, "  ...leaving the pristine .bak untouched");
	ok(exists(bak1), "  ...and putting the newer copy in .bak.1");
	ok(save_read_hero(save, &back, NULL, err) && back.pBaseStr == 100,
	    "  ...and the edit landed");

	/* Without a backup it proceeds too. */
	edited.pBaseStr = 101;
	ok(save_commit(save, &edited, /*backup=*/0, err), "commit without a backup");
	ok(save_read_hero(save, &back, NULL, err) && back.pBaseStr == 101,
	    "the third edit landed");

	/* Committing to something that is not a save must fail and change nothing. */
	const char *bogus = tmppath("bogus.sv");
	FILE *f = fopen(bogus, "wb");
	fputs("definitely not an MPQ archive", f);
	fclose(f);
	uint64_t bogus_before = file_hash(bogus);
	err[0] = '\0';
	ok(!save_commit(bogus, &edited, 0, err), "committing to a non-save fails");
	printf("        %s\n", err);
	ok(file_hash(bogus) == bogus_before, "  ...leaving the file untouched");

	ok(!save_commit(tmppath("does-not-exist.sv"), &edited, 0, err),
	    "committing to a missing file fails");
}

/* ------------------------------------------------------------------ */
/* 4. Name collisions                                                  */
/* ------------------------------------------------------------------ */

static void check_collisions(void)
{
	section("4. rename collisions");

	char dir[SAVE_PATH_MAX];
	snprintf(dir, sizeof(dir), "%s/coll", g_tmpdir);
	mkdir(dir, 0755);

	PkPlayerStruct a, b;
	base_hero(&a, "Aidan", PC_WARRIOR);
	base_hero(&b, "Moreina", PC_ROGUE);
	char pa[SAVE_PATH_MAX], pb[SAVE_PATH_MAX];
	snprintf(pa, sizeof(pa), "%s/single_0.sv", dir);
	snprintf(pb, sizeof(pb), "%s/single_1.sv", dir);
	make_save(pa, &a);
	make_save(pb, &b);

	char other[256] = { 0 };
	ok(save_name_collides(pa, "Moreina", other, sizeof(other)),
	    "renaming Aidan to Moreina collides");
	ok(strcmp(other, "single_1.sv") == 0, "  ...and names the conflicting file");

	ok(!save_name_collides(pa, "Aidan", other, sizeof(other)),
	    "a character does not collide with itself");
	ok(!save_name_collides(pa, "Griswold", other, sizeof(other)),
	    "an unused name does not collide");
	ok(save_name_collides(pa, "moreina", other, sizeof(other)),
	    "the check is case-insensitive, as the game's own lookup is");
}

/* ------------------------------------------------------------------ */
/* 5. Real save (opt-in)                                               */
/* ------------------------------------------------------------------ */

static void check_real_save(void)
{
	section("5. real save");

	const char *path = getenv("BUTCHER_SAVE");
	if (path == NULL || path[0] == '\0') {
		printf("  SKIP  set BUTCHER_SAVE to commit against a copy of a real save\n");
		return;
	}

	/* Work on a copy; the real save is never opened for writing. */
	const char *copy = tmppath("real_commit.sv");
	FILE *in = fopen(path, "rb");
	FILE *out = fopen(copy, "wb");
	if (in == NULL || out == NULL) {
		ok(0, "copy the real save");
		if (in)
			fclose(in);
		if (out)
			fclose(out);
		return;
	}
	char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(in);
	fclose(out);
	chmod(copy, 0644);

	char bak[SAVE_PATH_MAX];
	snprintf(bak, sizeof(bak), "%s.bak", copy);
	remove(bak);

	char err[MPQ_ERR_LEN] = { 0 };
	PkPlayerStruct h;
	ok(save_read_hero(copy, &h, NULL, err), "read a real character");

	PkPlayerStruct edited = h;
	edited.pBaseVit = (BYTE)(h.pBaseVit == 40 ? 41 : 40);

	ok(save_commit(copy, &edited, 1, err), "commit an edit to a real archive");
	ok(exists(bak), "a backup was written");

	PkPlayerStruct back;
	ok(save_read_hero(copy, &back, NULL, err)
	        && memcmp(&back, &edited, sizeof(back)) == 0,
	    "the real save round-trips the edit exactly");

	/* And the backup still holds the original character. */
	PkPlayerStruct from_bak;
	ok(save_read_hero(bak, &from_bak, NULL, err)
	        && memcmp(&from_bak, &h, sizeof(h)) == 0,
	    "the backup still holds the character as it was");
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

	printf("butcher -- shared save utilities\n");

	check_game_in_progress();
	check_discovery();
	check_backup();
	check_commit();
	check_collisions();
	check_real_save();

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
