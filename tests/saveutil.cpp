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

/** Write a one-file save archive containing this character. */
static int make_save(const char *path, const PkPlayerStruct *hero)
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

	BYTE body[4096];
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
	Encrypt((DWORD *)block, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY), Hash("(block table)", 3));
	Encrypt((DWORD *)hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), Hash("(hash table)", 3));

	FILE *f = fopen(path, "wb");
	int rc = -1;
	if (f != NULL) {
		fwrite(&hdr, sizeof(hdr), 1, f);
		fwrite(block, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY), 1, f);
		fwrite(hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), 1, f);
		fwrite(body, offs[1], 1, f);
		fclose(f);
		chmod(path, 0644);
		rc = 0;
	}
	free(block);
	free(hash);
	return rc;
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

	uint64_t before = file_hash(save);
	char err[MPQ_ERR_LEN] = { 0 };

	ok(save_backup(save, err), "first backup is written");
	ok(exists(bak), "the .bak file exists");
	ok(file_hash(bak) == before, "and is a faithful copy");

	/*
	 * The second backup must refuse. Overwriting would replace the pristine
	 * original with an already-edited one -- exactly when you need it most.
	 */
	err[0] = '\0';
	ok(!save_backup(save, err), "a second backup refuses rather than overwrite");
	printf("        %s\n", err);
	ok(file_hash(bak) == before, "the original backup is untouched");
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
	char bak[SAVE_PATH_MAX];
	snprintf(bak, sizeof(bak), "%s.bak", save);
	remove(bak);

	PkPlayerStruct edited = h;
	edited.pBaseStr = 99;
	edited.pGold = 0;

	ok(save_commit(save, &edited, /*backup=*/1, err), "commit with a backup");
	ok(exists(bak), "the backup was written");

	PkPlayerStruct back;
	ok(save_read_hero(save, &back, NULL, err), "the save reads back");
	ok(back.pBaseStr == 99, "the edit landed");
	ok(memcmp(&back, &edited, sizeof(back)) == 0, "the whole character matches");

	/* A second commit must not clobber the first backup. */
	edited.pBaseStr = 100;
	err[0] = '\0';
	ok(!save_commit(save, &edited, /*backup=*/1, err),
	    "a second backed-up commit refuses, protecting the pristine copy");
	ok(save_read_hero(save, &back, NULL, err) && back.pBaseStr == 99,
	    "  ...and the save is left as it was");

	/* Without a backup it proceeds. */
	ok(save_commit(save, &edited, /*backup=*/0, err), "commit without a backup");
	ok(save_read_hero(save, &back, NULL, err) && back.pBaseStr == 100,
	    "the second edit landed");

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

	check_discovery();
	check_backup();
	check_commit();
	check_collisions();
	check_real_save();

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
