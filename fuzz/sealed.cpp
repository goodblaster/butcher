/**
 * @file sealed.cpp
 *
 * Fuzz what lies behind the codec's checksum, through the real file path.
 *
 * The `save` harness mutates an archive's bytes, and any mutation that lands
 * in an encoded member fails that member's checksum -- so it never gets past
 * codec_decode, and everything after it goes untested. The `game` harness
 * tests the patcher, but hands it a buffer directly and so skips game_read
 * and the commit around it. Neither could have found a decoded length that
 * ran past its buffer, which is the bug this harness was written after: the
 * length comes from the codec's trailer, and the checksum does not cover the
 * trailer.
 *
 * So this one mutates the *decoded* contents, then seals them: it encodes
 * the result with a correct checksum, builds a real archive around it, and
 * runs the same calls an edit makes. Damage arrives with a valid signature,
 * the way a deliberately crafted save's would.
 *
 * Input layout: four bytes that tamper with the trailers after sealing, a
 * PkPlayerStruct, then the decoded saved game.
 */
#include "fuzz.h"

#include "../src/gamefile.h"
#include "../src/hero.h"
#include "../src/saveutil.h"

#include "../third_party/devilution/Source/encrypt.h"

#include <unistd.h>

const char *const fuzz_name = "sealed";

/* Offsets within the codec's 8-byte trailer (Source/codec.cpp). */
#define TRAILER_ERROR 4
#define TRAILER_LAST_CHUNK 5

/*
 * The four tamper bytes. Each of the last three acts whenever it is nonzero,
 * with no separate switch to throw first: the blunt driver changes a byte or
 * two per input, and a tamper that needed two particular bytes changed at
 * once would almost never happen.
 *
 *   input[0]  flags, below
 *   input[1]  nonzero: the game's last_chunk_size becomes this
 *   input[2]  nonzero: the game's error byte becomes this
 *   input[3]  nonzero: the hero's last_chunk_size becomes this
 */
#define TAMPER_GAME_MULTI 0x01 /* seal the game with the multiplayer password */
#define TAMPER_NO_GAME 0x02    /* leave the saved game out altogether */

/* Enough for a real saved game several times over; past this an input is
 * only making the run slower. */
#define MAX_GAME (512 * 1024)

static const char *scratch_path(void)
{
	static char path[512];
	if (path[0] == '\0') {
		const char *dir = getenv("TMPDIR");
		snprintf(path, sizeof(path), "%s/butcher-fuzz-sealed-%d.hsv",
		    dir != NULL ? dir : "/tmp", (int)getpid());
	}
	return path;
}

static void put32(uint8_t *p, int v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/** Encode @p plain for the save, with a checksum that is correct. */
static std::vector<uint8_t> seal(const uint8_t *plain, size_t len, const char *password)
{
	DWORD enc = codec_get_encoded_len((DWORD)len);
	/* codec_encode writes its trailer through a struct pointer. */
	std::vector<DWORD> aligned((enc + 3) / 4 + 1, 0);
	BYTE *buf = (BYTE *)aligned.data();
	memcpy(buf, plain, len);
	codec_encode(buf, (DWORD)len, (int)enc, password);
	return std::vector<uint8_t>(buf, buf + enc);
}

/** Append one member's on-disk body: sector table, then imploded sectors. */
static DWORD append_body(std::vector<uint8_t> &data, const std::vector<uint8_t> &member)
{
	DWORD len = (DWORD)member.size();
	DWORD nsec = (len + MPQ_SECTOR_SIZE - 1) / MPQ_SECTOR_SIZE;
	size_t start = data.size();
	DWORD pos = (nsec + 1) * (DWORD)sizeof(DWORD);
	data.resize(start + pos);

	for (DWORD s = 0; s < nsec; s++) {
		DWORD raw = len - s * MPQ_SECTOR_SIZE;
		if (raw > MPQ_SECTOR_SIZE)
			raw = MPQ_SECTOR_SIZE;
		BYTE sector[MPQ_SECTOR_SIZE];
		memcpy(sector, member.data() + s * MPQ_SECTOR_SIZE, raw);
		int stored = PkwareCompress(sector, (int)raw);
		memcpy(data.data() + start + s * sizeof(DWORD), &pos, sizeof(DWORD));
		data.insert(data.end(), sector, sector + stored);
		pos += (DWORD)stored;
	}
	memcpy(data.data() + start + nsec * sizeof(DWORD), &pos, sizeof(DWORD));
	return pos;
}

/** Write an archive holding `hero` and, if @p game is non-NULL, `game`. */
static int write_archive(const std::vector<uint8_t> &hero, const std::vector<uint8_t> *game)
{
	std::vector<_BLOCKENTRY> block(MPQ_INDEX_ENTRIES);
	std::vector<_HASHENTRY> hash(MPQ_INDEX_ENTRIES);
	memset(block.data(), 0, MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY));
	memset(hash.data(), 0xFF, MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY));
	InitHash();

	std::vector<uint8_t> data;
	const char *names[2] = { "hero", "game" };
	const std::vector<uint8_t> *members[2] = { &hero, game };

	for (int m = 0; m < 2; m++) {
		if (members[m] == NULL)
			continue;
		DWORD offset = MPQ_DATA_OFFSET + (DWORD)data.size();
		DWORD alloc = append_body(data, *members[m]);
		block[m].offset = (int)offset;
		block[m].sizealloc = (int)alloc;
		block[m].sizefile = (int)members[m]->size();
		block[m].flags = (int)(MPQ_FLAG_EXISTS | MPQ_FLAG_IMPLODE);

		DWORD idx = Hash(names[m], 0) & (MPQ_INDEX_ENTRIES - 1);
		while (hash[idx].block != -1)
			idx = (idx + 1) & (MPQ_INDEX_ENTRIES - 1);
		hash[idx].hashcheck[0] = (int)Hash(names[m], 1);
		hash[idx].hashcheck[1] = (int)Hash(names[m], 2);
		hash[idx].lcid = 0;
		hash[idx].block = m;
	}

	_FILEHEADER hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.signature = (int)MPQ_SIGNATURE;
	hdr.headersize = 32;
	hdr.filesize = (int)(MPQ_DATA_OFFSET + data.size());
	hdr.sectorsizeid = 3;
	hdr.hashoffset = MPQ_HASH_OFFSET;
	hdr.blockoffset = MPQ_BLOCK_OFFSET;
	hdr.hashcount = MPQ_INDEX_ENTRIES;
	hdr.blockcount = MPQ_INDEX_ENTRIES;

	Encrypt((DWORD *)block.data(), MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY),
	    Hash("(block table)", 3));
	Encrypt((DWORD *)hash.data(), MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY),
	    Hash("(hash table)", 3));

	FILE *f = fopen(scratch_path(), "wb");
	if (f == NULL)
		return 0;
	int ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1
	    && fwrite(block.data(), MPQ_INDEX_ENTRIES * sizeof(_BLOCKENTRY), 1, f) == 1
	    && fwrite(hash.data(), MPQ_INDEX_ENTRIES * sizeof(_HASHENTRY), 1, f) == 1
	    && (data.empty() || fwrite(data.data(), data.size(), 1, f) == 1);
	return fclose(f) == 0 && ok;
}

/* The same character and saved game the `game` harness starts from, behind
 * four tamper bytes that leave both trailers alone. */
void fuzz_make_seed(std::vector<uint8_t> &out)
{
	PkPlayerStruct h;
	char err[HERO_ERR_LEN];
	hero_create(&h, FLAVOR_HELLFIRE, 0, "Fuzzer", err);
	hero_set_gold(&h, FLAVOR_HELLFIRE, 12000, err);

	const long name_at = 43 + 25 * 8 + 320;
	const long list_at = name_at + 600;
	const long numinv_at = list_at + (long)NUM_INV_GRID_ELEM * GAME_ITEM_SIZE;
	const long grid_at = numinv_at + 4;
	std::vector<uint8_t> g((size_t)(grid_at + NUM_INV_GRID_ELEM + 64), 0);

	memcpy(g.data(), "HELF", 4);
	uint8_t *rec = g.data() + name_at;
	memcpy(rec - 127, h.pSplLvl, 37);
	memcpy(rec - 127 + 37, h.pSplLvl2, 10);
	memcpy(rec - 56, &h.pMemSpells, 8);
	memcpy(rec, h.pName, PLR_NAME_LEN);
	rec[32] = (uint8_t)h.pClass;
	put32(rec + 40, h.pBaseStr);
	put32(rec + 48, h.pBaseMag);
	put32(rec + 56, h.pBaseDex);
	put32(rec + 64, h.pBaseVit);
	put32(rec + 68, h.pStatPts);
	put32(rec + 80, h.pHPBase);
	put32(rec + 84, h.pMaxHPBase);
	put32(rec + 100, h.pManaBase);
	put32(rec + 104, h.pMaxManaBase);
	rec[120] = (uint8_t)h.pLevel;
	put32(rec + 124, h.pExperience);
	put32(rec + 140, h.pGold);
	for (int i = 0; i < h._pNumInv; i++) {
		uint8_t *it = g.data() + list_at + (size_t)i * GAME_ITEM_SIZE;
		put32(it + GAME_ITEM_SEED, (int)h.InvList[i].iSeed);
		put32(it + GAME_ITEM_ITYPE, h.InvList[i].idx == IDI_GOLD ? ITYPE_GOLD : ITYPE_MISC);
		put32(it + GAME_ITEM_IVALUE, h.InvList[i].idx == IDI_GOLD ? h.InvList[i].wValue : 0);
	}
	put32(g.data() + numinv_at, h._pNumInv);
	memcpy(g.data() + grid_at, h.InvGrid, NUM_INV_GRID_ELEM);

	out.assign(4, 0);
	out.insert(out.end(), (const uint8_t *)&h, (const uint8_t *)&h + sizeof(h));
	out.insert(out.end(), g.begin(), g.end());
}

/** Seal one input with the given tamper bytes and run an edit against it. */
static void run_one(const uint8_t *tamper, const uint8_t *data, size_t size)
{
	const uint8_t *hero_plain = data + 4;
	const uint8_t *game_plain = hero_plain + sizeof(PkPlayerStruct);
	size_t game_len = size - 4 - sizeof(PkPlayerStruct);
	if (game_len > MAX_GAME)
		game_len = MAX_GAME;

	std::vector<uint8_t> hero = seal(hero_plain, sizeof(PkPlayerStruct), SAVE_PASSWORD_SINGLE);
	if (tamper[3] != 0)
		hero[hero.size() - 8 + TRAILER_LAST_CHUNK] = tamper[3];

	/* codec_encode cannot seal nothing, so an empty game means none. */
	bool with_game = game_len > 0 && !(tamper[0] & TAMPER_NO_GAME);
	std::vector<uint8_t> game;
	if (with_game) {
		game = seal(game_plain, game_len,
		    (tamper[0] & TAMPER_GAME_MULTI) ? SAVE_PASSWORD_MULTI : SAVE_PASSWORD_SINGLE);
		if (tamper[1] != 0)
			game[game.size() - 8 + TRAILER_LAST_CHUNK] = tamper[1];
		if (tamper[2] != 0)
			game[game.size() - 8 + TRAILER_ERROR] = tamper[2];
	}

	if (!write_archive(hero, with_game ? &game : NULL))
		return;
	const char *path = scratch_path();

	/* Reading, the way `inspect` does. */
	char err[MPQ_ERR_LEN];
	PkPlayerStruct old;
	int multi = 0;
	int have_hero = save_read_hero(path, &old, &multi, err);

	DWORD glen = 0;
	int present = 0;
	BYTE *g = game_read(path, &glen, &present, err);
	if (g != NULL && have_hero) {
		GameLoc loc;
		game_locate(g, glen, &old, &loc, err);
	}
	free(g);

	/* Writing, the way an edit does: change the character and commit it,
	 * which patches the saved game and rewrites the archive. */
	if (have_hero) {
		PkPlayerStruct neu = old;
		char herr[HERO_ERR_LEN];
		neu.pBaseStr = (BYTE)(neu.pBaseStr + 1);
		hero_set_gold(&neu, FLAVOR_HELLFIRE, hero_gold_in_stacks(&old) + 7000, herr);
		save_commit(path, &neu, /*backup=*/0, err);
	}

	remove(path);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 4 + sizeof(PkPlayerStruct))
		return 0;

	/* As the input asks. */
	run_one(data, data, size);

	/*
	 * And once more with the saved game's trailer claiming the longest last
	 * block a byte can express, whatever the input said. That one value is
	 * what turns a decoded length into an overrun, and leaving it to a random
	 * mutation landing on input[1] meant the standalone driver's 20000 runs
	 * went by without ever trying it.
	 */
	if (data[1] != 255) {
		const uint8_t longest[4] = { data[0], 255, data[2], data[3] };
		run_one(longest, data, size);
	}
	return 0;
}
