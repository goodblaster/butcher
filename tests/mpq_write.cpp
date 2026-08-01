/**
 * @file write_check.cpp
 *
 * Phase 3 gate: the MPQ writer in savefile.cpp.
 *
 * The properties that matter, in order of how badly a violation would hurt:
 *
 *   1. A failed write leaves the original byte-for-byte intact, with no
 *      temporary files left behind.
 *   2. Files other than the one being replaced survive byte-for-byte.
 *   3. The hash table keeps its shape, including deleted entries -- dropping
 *      one shortens a probe chain and makes an unrelated file unreachable.
 *   4. Rewriting an unchanged archive reproduces it exactly.
 *   5. The result reads back as intended.
 */
#include "../src/savefile.h"

#include "../third_party/devilution/Source/encrypt.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

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

static long file_size(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

/** Count files in the scratch dir whose name contains ".tmp". */
static int count_temp_files(void)
{
	DIR *d = opendir(g_tmpdir);
	if (d == NULL)
		return -1;
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL)
		if (strstr(e->d_name, ".tmp") != NULL)
			n++;
	closedir(d);
	return n;
}

/* ------------------------------------------------------------------ */
/* Archive builder (same as phase 2)                                   */
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
	int rc = -1;

	memset(hash, 0xFF, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
	InitHash();

	for (int e = 0; e < count; e++) {
		DWORD file_len = entries[e].len;
		DWORD nsectors = (file_len + MPQ_SECTOR_SIZE - 1) / MPQ_SECTOR_SIZE;
		DWORD table_bytes = (nsectors + 1) * sizeof(DWORD);
		BYTE *body = (BYTE *)malloc(table_bytes + nsectors * MPQ_SECTOR_SIZE * 2);
		DWORD *offs = (DWORD *)body;
		DWORD pos = table_bytes;

		for (DWORD s = 0; s < nsectors; s++) {
			DWORD raw = file_len - s * MPQ_SECTOR_SIZE;
			if (raw > MPQ_SECTOR_SIZE)
				raw = MPQ_SECTOR_SIZE;
			BYTE sector[MPQ_SECTOR_SIZE];
			memcpy(sector, entries[e].data + s * MPQ_SECTOR_SIZE, raw);
			int stored = PkwareCompress(sector, (int)raw);
			offs[s] = pos;
			memcpy(body + pos, sector, (size_t)stored);
			pos += (DWORD)stored;
		}
		offs[nsectors] = pos;

		DWORD block_off = MPQ_DATA_OFFSET + data_len;
		data = (BYTE *)realloc(data, data_len + pos);
		memcpy(data + data_len, body, pos);
		data_len += pos;
		free(body);

		block[e].offset = (int)block_off;
		block[e].sizealloc = (int)pos;
		block[e].sizefile = (int)file_len;
		block[e].flags = (int)(MPQ_FLAG_EXISTS | MPQ_FLAG_IMPLODE);

		DWORD idx = Hash(entries[e].name, 0) & 0x7FF;
		while (hash[idx].block != -1)
			idx = (idx + 1) & 0x7FF;
		hash[idx].hashcheck[0] = (int)Hash(entries[e].name, 1);
		hash[idx].hashcheck[1] = (int)Hash(entries[e].name, 2);
		hash[idx].lcid = 0;
		hash[idx].block = e;
	}

	long total = MPQ_DATA_OFFSET + (long)data_len;
	memset(&hdr, 0, sizeof(hdr));
	hdr.signature = (int)MPQ_SIGNATURE;
	hdr.headersize = 32;
	hdr.filesize = (int)total;
	hdr.version = 0;
	hdr.sectorsizeid = 3;
	hdr.hashoffset = MPQ_HASH_OFFSET;
	hdr.blockoffset = MPQ_BLOCK_OFFSET;
	hdr.hashcount = MPQ_INDEX_ENTRIES;
	hdr.blockcount = MPQ_INDEX_ENTRIES;

	Encrypt((DWORD *)block, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY), Hash("(block table)", 3));
	Encrypt((DWORD *)hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), Hash("(hash table)", 3));

	FILE *f = fopen(path, "wb");
	if (f == NULL)
		goto done;
	if (!fwrite(&hdr, sizeof(hdr), 1, f))
		goto close;
	if (!fwrite(block, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY), 1, f))
		goto close;
	if (!fwrite(hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), 1, f))
		goto close;
	if (data_len && !fwrite(data, data_len, 1, f))
		goto close;
	rc = 0;
close:
	fclose(f);
done:
	free(block);
	free(hash);
	free(data);
	return rc;
}

/* ------------------------------------------------------------------ */

static BYTE g_blob[1288];
static PkPlayerStruct g_plain;

static void make_hero(PkPlayerStruct *p, const char *name, int level, int gold)
{
	memset(p, 0, sizeof(*p));
	strcpy(p->pName, name);
	p->pClass = PC_ROGUE;
	p->pLevel = (char)level;
	p->pBaseStr = 30;
	p->pBaseMag = 25;
	p->pBaseDex = 60;
	p->pBaseVit = 28;
	p->pExperience = 90000;
	p->pGold = gold;
	p->pHPBase = 70 << 6;
	p->pMaxHPBase = 70 << 6;
	p->pManaBase = 45 << 6;
	p->pMaxManaBase = 45 << 6;
	p->plrlevel = 6;
	for (int i = 0; i < NUM_INVLOC; i++)
		p->InvBody[i].idx = 0xFFFF;
	for (int i = 0; i < NUM_INV_GRID_ELEM; i++)
		p->InvList[i].idx = 0xFFFF;
	for (int i = 0; i < MAXBELTITEMS; i++)
		p->SpdList[i].idx = 0xFFFF;
}

static void encode_hero(const PkPlayerStruct *p, BYTE *out1288)
{
	memset(out1288, 0, 1288);
	memcpy(out1288, p, sizeof(*p));
	codec_encode(out1288, sizeof(*p), 1288, SAVE_PASSWORD_SINGLE);
}

/* ------------------------------------------------------------------ */
/* 1. Idempotent rewrite                                               */
/* ------------------------------------------------------------------ */

static void check_idempotent(void)
{
	section("1. rewriting an unchanged archive reproduces it exactly");

	Entry e = { "hero", g_blob, 1288 };
	const char *path = tmppath("idem.sv");
	build_archive(path, &e, 1);

	uint64_t before = file_hash(path);
	long size_before = file_size(path);

	char err[MPQ_ERR_LEN] = { 0 };
	int r = save_write_hero(path, &g_plain, err);
	ok(r, "save_write_hero succeeds");
	if (!r) {
		printf("        %s\n", err);
		return;
	}

	ok(file_size(path) == size_before, "file size unchanged");
	ok(file_hash(path) == before, "file is byte-for-byte identical");
	ok(count_temp_files() == 0, "no temporary files left behind");

	/*
	 * mkstemp creates at 0600 and rename() carries the mode across, so a
	 * naive implementation silently strips group/other permissions off the
	 * user's save.
	 */
	{
		struct stat st;
		ok(stat(path, &st) == 0 && (st.st_mode & 07777) == 0644,
		    "file permissions preserved (0644, not mkstemp's 0600)");
	}

	/* And again, to be sure it is stable under repetition. */
	ok(save_write_hero(path, &g_plain, err), "second rewrite succeeds");
	ok(file_hash(path) == before, "still byte-for-byte identical");
}

/* ------------------------------------------------------------------ */
/* 2. Actual edits land                                                */
/* ------------------------------------------------------------------ */

static void check_edit(void)
{
	section("2. edits are written and read back");

	Entry e = { "hero", g_blob, 1288 };
	const char *path = tmppath("edit.sv");
	build_archive(path, &e, 1);

	PkPlayerStruct edited = g_plain;
	edited.pLevel = 42;
	edited.pGold = 123456;
	edited.pBaseStr = 99;
	strcpy(edited.pName, "Griswold");

	char err[MPQ_ERR_LEN] = { 0 };
	ok(save_write_hero(path, &edited, err), "write edited hero");

	PkPlayerStruct back;
	ok(save_read_hero(path, &back, NULL, err), "read it back");
	ok(back.pLevel == 42, "level changed");
	ok(back.pGold == 123456, "gold changed");
	ok(back.pBaseStr == 99, "strength changed");
	ok(strcmp(back.pName, "Griswold") == 0, "name changed");
	ok(memcmp(&back, &edited, sizeof(back)) == 0, "whole struct matches what was written");

	/* The archive must still satisfy the reader's strict header checks. */
	MpqArchive *a = mpq_open(path, err);
	ok(a != NULL, "result still passes strict header validation");
	if (a != NULL) {
		ok(mpq_has_file(a, "hero"), "hero still resolvable");
		mpq_close(a);
	}
}

/* ------------------------------------------------------------------ */
/* 3. Untouched files survive                                          */
/* ------------------------------------------------------------------ */

static void check_neighbours(void)
{
	section("3. other files in the archive are preserved byte-for-byte");

	DWORD big = 9000;
	BYTE *zeros = (BYTE *)calloc(1, big);
	BYTE *noise = (BYTE *)malloc(big);
	uint32_t s = 999;
	for (DWORD i = 0; i < big; i++) {
		s = s * 1103515245u + 12345u;
		noise[i] = (BYTE)(s >> 16);
	}
	BYTE tiny[5] = { 9, 8, 7, 6, 5 };

	/* hero deliberately in the middle, so neighbours shift on both sides
	 * if the layout logic is wrong. */
	Entry entries[4] = {
		{ "perml00", zeros, big },
		{ "hero", g_blob, 1288 },
		{ "game", noise, big },
		{ "perms03", tiny, sizeof(tiny) },
	};
	const char *path = tmppath("neighbours.sv");
	build_archive(path, entries, 4);

	char err[MPQ_ERR_LEN] = { 0 };
	PkPlayerStruct edited = g_plain;
	edited.pGold = 55555;
	ok(save_write_hero(path, &edited, err), "patch hero in a 4-file archive");

	MpqArchive *a = mpq_open(path, err);
	ok(a != NULL, "reopens");
	if (a == NULL) {
		printf("        %s\n", err);
		free(zeros);
		free(noise);
		return;
	}

	for (int i = 0; i < 4; i++) {
		if (strcmp(entries[i].name, "hero") == 0)
			continue;
		DWORD len = 0;
		BYTE *got = mpq_read_file(a, entries[i].name, &len, err);
		char label[128];
		snprintf(label, sizeof(label), "\"%s\" unchanged", entries[i].name);
		if (got == NULL) {
			ok(0, label);
			printf("        %s\n", err);
			continue;
		}
		ok(len == entries[i].len && memcmp(got, entries[i].data, len) == 0, label);
		free(got);
	}

	DWORD hl = 0;
	BYTE *h = mpq_read_file(a, "hero", &hl, err);
	ok(h != NULL && hl == 1288, "hero is present and the right size");
	free(h);
	mpq_close(a);

	PkPlayerStruct back;
	ok(save_read_hero(path, &back, NULL, err) && back.pGold == 55555,
	    "the edit landed");

	free(zeros);
	free(noise);
}

/* ------------------------------------------------------------------ */
/* 4. Deleted hash entries survive                                     */
/* ------------------------------------------------------------------ */

/*
 * A deleted hash entry (block == -2) is a tombstone: it keeps a probe chain
 * intact. If a rewrite were to clear it to -1, any file that had probed past
 * it would become unreachable -- silent data loss that only shows up later.
 *
 * This builds exactly that shape by hand: hero's home slot holds a tombstone
 * and hero itself sits one slot further along.
 */
static void plant_tombstone(const char *path)
{
	FILE *f = fopen(path, "r+b");
	if (f == NULL)
		return;

	_HASHENTRY *hash = (_HASHENTRY *)malloc(MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
	fseek(f, MPQ_HASH_OFFSET, SEEK_SET);
	if (fread(hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), 1, f) != 1) {
		fclose(f);
		free(hash);
		return;
	}
	InitHash();
	Decrypt((DWORD *)hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), Hash("(hash table)", 3));

	DWORD home = Hash("hero", 0) & 0x7FF;
	DWORD next = (home + 1) & 0x7FF;

	hash[next] = hash[home];  /* move hero one slot along */
	hash[home].hashcheck[0] = 0x11111111;
	hash[home].hashcheck[1] = 0x22222222;
	hash[home].lcid = 0;
	hash[home].block = -2; /* tombstone */

	Encrypt((DWORD *)hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), Hash("(hash table)", 3));
	fseek(f, MPQ_HASH_OFFSET, SEEK_SET);
	fwrite(hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), 1, f);
	fclose(f);
	free(hash);
}

static int tombstone_present(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return 0;
	_HASHENTRY *hash = (_HASHENTRY *)malloc(MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
	fseek(f, MPQ_HASH_OFFSET, SEEK_SET);
	if (fread(hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), 1, f) != 1) {
		fclose(f);
		free(hash);
		return 0;
	}
	InitHash();
	Decrypt((DWORD *)hash, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), Hash("(hash table)", 3));
	DWORD home = Hash("hero", 0) & 0x7FF;
	int r = (hash[home].block == -2);
	fclose(f);
	free(hash);
	return r;
}

static void check_tombstone(void)
{
	section("4. deleted hash entries (tombstones) survive a rewrite");

	Entry e = { "hero", g_blob, 1288 };
	const char *path = tmppath("tomb.sv");
	build_archive(path, &e, 1);
	plant_tombstone(path);

	ok(tombstone_present(path), "tombstone planted at hero's home slot");

	char err[MPQ_ERR_LEN] = { 0 };
	PkPlayerStruct hero;
	ok(save_read_hero(path, &hero, NULL, err), "hero still readable past the tombstone");

	PkPlayerStruct edited = g_plain;
	edited.pGold = 4242;
	ok(save_write_hero(path, &edited, err), "patch succeeds");

	ok(tombstone_present(path), "tombstone still present after the rewrite");

	PkPlayerStruct back;
	ok(save_read_hero(path, &back, NULL, err) && back.pGold == 4242,
	    "hero still reachable and updated");
}

/* ------------------------------------------------------------------ */
/* 5. Failures leave everything alone                                  */
/* ------------------------------------------------------------------ */

static void check_failure_atomicity(void)
{
	section("5. failed writes leave the original untouched");

	char err[MPQ_ERR_LEN] = { 0 };

	/* Replacing a file that does not exist. */
	Entry e = { "hero", g_blob, 1288 };
	const char *path = tmppath("fail1.sv");
	build_archive(path, &e, 1);
	uint64_t before = file_hash(path);

	ok(!mpq_replace_file(path, "nosuch", g_blob, 1288, err),
	    "replacing an absent file is refused");
	printf("        %s\n", err);
	ok(file_hash(path) == before, "original unchanged");
	ok(count_temp_files() == 0, "no temporary files left behind");

	/* A corrupt archive must be refused before anything is written. */
	const char *bad = tmppath("fail2.sv");
	build_archive(bad, &e, 1);
	FILE *f = fopen(bad, "r+b");
	fseek(f, 0, SEEK_SET);
	fputc(0x00, f); /* break the signature */
	fclose(f);
	uint64_t bad_before = file_hash(bad);

	ok(!save_write_hero(bad, &g_plain, err), "corrupt archive is refused");
	printf("        %s\n", err);
	ok(file_hash(bad) == bad_before, "corrupt archive unchanged");
	ok(count_temp_files() == 0, "no temporary files left behind");

	/* Nonexistent path. */
	ok(!save_write_hero(tmppath("does_not_exist.sv"), &g_plain, err),
	    "missing file is refused");
	ok(count_temp_files() == 0, "no temporary files left behind");
}

/* ------------------------------------------------------------------ */
/* 6. Size changes                                                     */
/* ------------------------------------------------------------------ */

/*
 * hero is always 1288 bytes, but mpq_replace_file is general. Shrinking and
 * growing a file exercises the relayout path that a fixed-size hero never
 * reaches -- worth covering before anything else starts using it.
 */
static void check_resize(void)
{
	section("6. replacing a file with a different size");

	DWORD mid = 6000;
	BYTE *filler = (BYTE *)malloc(mid);
	for (DWORD i = 0; i < mid; i++)
		filler[i] = (BYTE)(i * 7);

	BYTE small[64];
	memset(small, 0xAB, sizeof(small));

	Entry entries[3] = {
		{ "game", filler, mid },
		{ "hero", g_blob, 1288 },
		{ "perml01", filler, mid },
	};
	const char *path = tmppath("resize.sv");

	/* Grow "game" from 6000 to 20000 bytes. */
	build_archive(path, entries, 3);
	DWORD grown_len = 20000;
	BYTE *grown = (BYTE *)malloc(grown_len);
	for (DWORD i = 0; i < grown_len; i++)
		grown[i] = (BYTE)(i * 13 + 5);

	char err[MPQ_ERR_LEN] = { 0 };
	ok(mpq_replace_file(path, "game", grown, grown_len, err), "grow a file");

	MpqArchive *a = mpq_open(path, err);
	ok(a != NULL, "reopens after growth");
	if (a != NULL) {
		DWORD len = 0;
		BYTE *got = mpq_read_file(a, "game", &len, err);
		ok(got != NULL && len == grown_len && memcmp(got, grown, grown_len) == 0,
		    "grown file reads back correctly");
		free(got);
		got = mpq_read_file(a, "perml01", &len, err);
		ok(got != NULL && len == mid && memcmp(got, filler, mid) == 0,
		    "neighbour after it is intact");
		free(got);
		got = mpq_read_file(a, "hero", &len, err);
		ok(got != NULL && len == 1288 && memcmp(got, g_blob, 1288) == 0,
		    "hero is intact");
		free(got);
		mpq_close(a);
	}

	/* Shrink it to 64 bytes. */
	ok(mpq_replace_file(path, "game", small, sizeof(small), err), "shrink a file");
	a = mpq_open(path, err);
	ok(a != NULL, "reopens after shrink");
	if (a != NULL) {
		DWORD len = 0;
		BYTE *got = mpq_read_file(a, "game", &len, err);
		ok(got != NULL && len == sizeof(small) && memcmp(got, small, sizeof(small)) == 0,
		    "shrunk file reads back correctly");
		free(got);
		got = mpq_read_file(a, "perml01", &len, err);
		ok(got != NULL && len == mid && memcmp(got, filler, mid) == 0,
		    "neighbour still intact");
		free(got);
		mpq_close(a);
	}
	ok(file_size(path) < MPQ_DATA_OFFSET + (long)(mid * 2 + 20000),
	    "archive was compacted, not merely appended to");

	free(filler);
	free(grown);
}

/* ------------------------------------------------------------------ */
/* 7. Real save (opt-in, read-modify-read on a copy)                   */
/* ------------------------------------------------------------------ */

/**
 * Count live files and free-space entries by reading the block table.
 * A free-space entry is one with an offset but no flags and no file size
 * (Source/mpqapi.cpp:186) -- a gap the game left behind.
 */
static void count_blocks(const char *path, int *live, int *gaps)
{
	*live = 0;
	*gaps = 0;

	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return;
	_BLOCKENTRY *b = (_BLOCKENTRY *)malloc(MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY));
	if (fseek(f, MPQ_BLOCK_OFFSET, SEEK_SET) == 0
	    && fread(b, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY), 1, f) == 1) {
		InitHash();
		Decrypt((DWORD *)b, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY),
		    Hash("(block table)", 3));
		for (int i = 0; i < MPQ_INDEX_ENTRIES; i++) {
			if ((DWORD)b[i].flags & MPQ_FLAG_EXISTS)
				(*live)++;
			else if (b[i].offset != 0)
				(*gaps)++;
		}
	}
	free(b);
	fclose(f);
}

static void check_real_save(void)
{
	section("7. real save");

	const char *path = getenv("BUTCHER_SAVE");
	if (path == NULL || path[0] == '\0') {
		printf("  SKIP  set BUTCHER_SAVE=/path/to/single_0.sv to exercise the\n");
		printf("        writer on a real archive (a COPY is made; the original is\n");
		printf("        never opened for writing)\n");
		return;
	}

	/* Work on a copy. The real save is never touched. */
	const char *copy = tmppath("real_copy.sv");
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

	uint64_t before = file_hash(copy);

	char err[MPQ_ERR_LEN] = { 0 };
	PkPlayerStruct hero;
	int multi = 0;
	if (!save_read_hero(copy, &hero, &multi, err)) {
		ok(0, "read the real save");
		printf("        %s\n", err);
		return;
	}
	ok(1, "read the real save");
	if (multi) {
		printf("  SKIP  archive uses the multiplayer password; writer targets "
		       "single-player only\n");
		return;
	}

	/*
	 * Byte-identity is the wrong assertion for a real archive. The writer
	 * rebuilds and compacts, so an archive carrying free-space entries --
	 * gaps left where the game rewrote a file in place -- legitimately comes
	 * back smaller. What must hold is that every file survives.
	 */
	long size_before = file_size(copy);
	int live_before = 0, gaps_before = 0;
	count_blocks(copy, &live_before, &gaps_before);
	printf("        %d files, %d free-space gaps, %ld bytes\n",
	    live_before, gaps_before, size_before);

	ok(save_write_hero(copy, &hero, err), "rewrite it unchanged");

	int live_after = 0, gaps_after = 0;
	count_blocks(copy, &live_after, &gaps_after);
	printf("        -> %d files, %d gaps, %ld bytes\n",
	    live_after, gaps_after, file_size(copy));

	ok(live_after == live_before, "every file survived the rewrite");
	ok(gaps_after == 0, "free space was reclaimed");

	if (gaps_before == 0) {
		ok(file_hash(copy) == before,
		    "already-compact archive rewrites byte-for-byte identically");
	} else {
		ok(file_size(copy) < size_before,
		    "archive with gaps was compacted (expected, not a fault)");
	}

	PkPlayerStruct back;
	ok(save_read_hero(copy, &back, NULL, err)
	        && memcmp(&back, &hero, sizeof(hero)) == 0,
	    "still decodes to the same character");

	/* Nothing left to reclaim, so a second rewrite must be byte-stable. */
	uint64_t settled = file_hash(copy);
	ok(save_write_hero(copy, &hero, err), "rewrite a second time");
	ok(file_hash(copy) == settled, "a compacted archive is then byte-stable");
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

	printf("Devilution character editor -- Phase 3 MPQ writer\n");
	printf("scratch dir: %s\n", g_tmpdir);

	make_hero(&g_plain, "Aidan", 18, 3000);
	encode_hero(&g_plain, g_blob);

	check_idempotent();
	check_edit();
	check_neighbours();
	check_tombstone();
	check_failure_atomicity();
	check_resize();
	check_real_save();

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
