/**
 * @file hellfire.cpp
 *
 * Hellfire's compile-time constants, exported for use by the rest of the
 * tool -- which is built for Diablo.
 *
 * This is the only translation unit compiled with -DHELLFIRE, so it is the
 * only place where defs.h and enums.h yield Hellfire's values. Taking them
 * from the real headers means they cannot drift; typing 52 by hand could.
 *
 * Safe because it exports nothing but integers. The packed save structs are
 * byte-identical between the two flavors (PkPlayerStruct is 1266 either way,
 * with every field at the same offset), and SpellData and ItemDataStruct
 * contain no conditional members, so no type crosses this boundary with a
 * different layout.
 */
#include "shim.h"

#include <stddef.h> /* offsetof */

extern const int hf_num_classes;
extern const int hf_max_spells;
extern const int hf_num_levels;
extern const int hf_max_lvls;

const int hf_num_classes = NUM_CLASSES;
const int hf_max_spells = MAX_SPELLS;
const int hf_num_levels = NUMLEVELS;
const int hf_max_lvls = MAX_LVLS;

/* The layout guarantee this whole approach rests on. */
CT_ASSERT(sizeof(PkPlayerStruct) == 1266, hf_pack_still_1266);
CT_ASSERT(sizeof(PkItemStruct) == 19, hf_item_still_19);
CT_ASSERT(offsetof(PkPlayerStruct, pName) == 16, hf_name_offset);
CT_ASSERT(offsetof(PkPlayerStruct, pGold) == 59, hf_gold_offset);
CT_ASSERT(offsetof(PkPlayerStruct, pSplLvl) == 79, hf_spllvl_offset);
CT_ASSERT(offsetof(PkPlayerStruct, pSplLvl2) == 1222, hf_spllvl2_offset);
CT_ASSERT(offsetof(PkPlayerStruct, InvList) == 257, hf_invlist_offset);
