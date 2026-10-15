/**
 * @file game.cpp
 *
 * Fuzz the saved-game patcher: game_locate's scans for the name and the
 * inventory anchor, and game_apply / game_apply_inventory writing into
 * whatever it found. This is the code that patches 200 KB of game state by
 * offset, so a bounds mistake here is the expensive kind.
 *
 * Input layout: a PkPlayerStruct, then the decoded saved game. Mutating the
 * character as well as the blob means the two can disagree in every way a
 * real save's two copies might.
 */
#include "fuzz.h"

#include "../src/gamefile.h"
#include "../src/hero.h"

const char *const fuzz_name = "game";

static void put32(uint8_t *p, int v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* The same shape tests/saveutil.cpp builds: magic, prologue, the record with
 * every field gamefile.cpp checks, then an inventory it can anchor on. */
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

	out.assign((const uint8_t *)&h, (const uint8_t *)&h + sizeof(h));
	out.insert(out.end(), g.begin(), g.end());
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < sizeof(PkPlayerStruct))
		return 0;

	PkPlayerStruct old;
	memcpy(&old, data, sizeof(old));
	DWORD glen = (DWORD)(size - sizeof(old));

	/* An exact-size heap copy, so ASan sees any read or write past the end. */
	BYTE *game = (BYTE *)malloc(glen == 0 ? 1 : glen);
	memcpy(game, data + sizeof(old), glen);

	GameLoc loc;
	char err[MPQ_ERR_LEN];
	if (game_locate(game, glen, &old, &loc, err)) {
		/* What an edit does: change the character, then write it in. */
		PkPlayerStruct neu = old;
		char herr[HERO_ERR_LEN];
		hero_set_gold(&neu, loc.hellfire ? FLAVOR_HELLFIRE : FLAVOR_DIABLO,
		    hero_gold_in_stacks(&old) + 7000, herr);
		game_apply(game, glen, &loc, &neu);
		game_apply_inventory(game, glen, &loc, &old, &neu, err);
	}
	free(game);
	return 0;
}
