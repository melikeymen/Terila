#pragma once

#include "version/mc_version.hpp"

// Behaviour that differs between protocol releases.
//
// A flag is defined only while the compiled target still behaves that way, so a use site asks
// `#ifdef TERILA_HAS_...` and never compares version numbers itself: the version knowledge
// stays here instead of spreading `#if 2603 < ...` through the tree.
//
// A new release appends one guarded block that lists only the differences:
//
//   new behaviour       #define TERILA_HAS_NEW_THING 1
//   changed behaviour   #undef  TERILA_HAS_OLD_THING
//                       #define TERILA_HAS_NEW_THING 1
//   dropped behaviour   #undef  TERILA_HAS_OLD_THING
//
// Flags set by the 26.3 block stay defined for every later target, so unchanged behaviour is
// inherited instead of being written again.

// ---- 26.3: the floor, i.e. everything this tree speaks today -----------------------------

// Configuration phase
#define TERILA_HAS_UPDATE_ENABLED_FEATURES 1
#define TERILA_HAS_SELECT_KNOWN_PACKS 1
#define TERILA_HAS_REGISTRY_DATA_PACKET 1

// Play phase framing
#define TERILA_HAS_CHUNK_BATCH 1             // chunk_batch_start / chunk_batch_finished
#define TERILA_HAS_LIGHT_MASK_BITSET 1       // light masks as BitSet.toByteArray()

// Packet names that were renamed with the payload left alone
#define TERILA_HAS_CHUNK_CACHE_CENTER 1      // set_chunk_cache_center, not set_center_chunk

// Item components
#define TERILA_HAS_SNBT_COMPONENTS 1         // components travel as SNBT text
#define TERILA_HAS_ENCHANTMENT_LEVELS 1      // enchantments={sharpness:255}

// ---- 26.4 and later ---------------------------------------------------------------------
// Each later release appends a guarded block, for example:
//
//   // ---- 26.4 ----
//   #if TERILA_MC_AT_LEAST(26, 4)
//   #  define TERILA_HAS_GAME_RULE_SYNC 1
//   #  undef  TERILA_HAS_CHUNK_CACHE_CENTER
//   #endif
//
// The guard matters: the block must not run when an older release is compiled after a newer
// one already exists.
