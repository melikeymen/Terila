# terila::version

Which Minecraft protocol a build speaks, and which behaviour that protocol implies.

Two headers, no translation unit: the numbers are `constexpr` and the differences are
preprocessor flags, so every use site is resolved while compiling.

- `mc_version.hpp` — the table of supported releases, the 26.3 floor, and the comparison
  macros `TERILA_MC_AT_LEAST(major, minor)` / `TERILA_MC_IS(major, minor)`.
- `features.hpp` — `TERILA_HAS_*` flags, one per behaviour that differs between releases.

## Choosing a target

```bash
cmake -B build -DTERILA_MC_TARGET=26.3
```

CMake turns `26.3` into `2603` and passes `TERILA_MC_TARGET=2603`. Without the option the
build uses the newest row in the table, which today is 26.3.

## The floor

26.3 is the oldest release this tree speaks, and that is enforced rather than documented:

- `TERILA_MC_TARGET` below `TERILA_MC_CODE(26, 3)` is an `#error`.
- A target with no row in `version::table` fails a `static_assert`.
- At startup the protocol recorded in `input/packets/ids.json` is compared with the build;
  a dump extracted from another release stops the server with the two versions in the log
  instead of speaking the wrong packet ids.

Nothing older than 26.3 is compiled in, so there are no fallbacks for it anywhere.

## Using a flag

```cpp
#include "version/features.hpp"

#ifdef TERILA_HAS_UPDATE_ENABLED_FEATURES
    send_update_enabled_features();
#endif
```

Sites ask about behaviour, never about version numbers: `#if TERILA_MC_AT_LEAST(27, 0)` in a
packet writer spreads release knowledge across the tree, while `#ifdef TERILA_HAS_CHUNK_BATCH`
keeps it in one file.

## Adding a release

Two edits, both additive:

1. Append one row to `version::table`:

   ```cpp
   {TERILA_MC_CODE(26, 4), "26.4", 780, 5050, "26.4"},
   ```

2. Append one guarded block to `features.hpp` listing **only what changed**:

   ```cpp
   // ---- 26.4 ----
   #if TERILA_MC_AT_LEAST(26, 4)
   #  define TERILA_HAS_GAME_RULE_SYNC 1   // new behaviour
   #  undef  TERILA_HAS_CHUNK_CACHE_CENTER   // renamed packet
   #  define TERILA_HAS_SET_CENTER_CHUNK 1
   #endif
   ```

Everything the previous blocks define stays defined for the new target, so unchanged
behaviour is inherited and never repeated. A flag that is not touched keeps its meaning for
every later release, which is exactly what "only add what changed" needs.

Code that has to branch inside one function keeps the two paths next to each other:

```cpp
#ifdef TERILA_HAS_SET_CENTER_CHUNK
    write_set_center_chunk(out, cx, cz);
#else
    write_set_chunk_cache_center(out, cx, cz);
#endif
```

## What is *not* here

This is compile-time selection: one binary speaks one protocol. Serving several client
versions from a single process needs runtime dispatch on the handshake protocol as well,
with the packet layouts of each release available at once. The version table and the flags
are the ground that dispatch would stand on, but the dispatch itself is not implemented.
