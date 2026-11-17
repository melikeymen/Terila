# terila::version

Which Minecraft protocol a build speaks, and which behaviour that protocol implies.

Two headers, no translation unit: the numbers are `constexpr` and the differences are
preprocessor flags, so every use site is resolved while compiling.

- `mc_version.hpp` — the table of supported releases, the 26.3 floor, and the comparison
  macros `TERILA_MC_CODE(major, minor)`, `TERILA_MC_AT_LEAST(major, minor)`, and
  `TERILA_MC_IS(major, minor)`.
- `features.hpp` — `TERILA_HAS_*` flags, one per behaviour that differs between releases.

## Choosing a target

```bash
cmake -B build -DTERILA_MC_TARGET=26.3
```

CMake turns `26.3` into `2603` with `TERILA_MC_CODE(26, 3)` and passes
`TERILA_MC_TARGET=2603`. `TERILA_MC_IS(26, 3)` expands to the comparison
`TERILA_MC_TARGET == TERILA_MC_CODE(26, 3)`, so it is true only when the selected target is
26.3. Both macros can be used in `#if` expressions. Without the CMake option, the build uses
the newest row in the table, which today is 26.3.

## Release table

Each `version::table` row has five fields:

```cpp
{TERILA_MC_CODE(26, 4), "26.4", 780, 5050, "26.4"}
```

In order, these are the comparison code, release name, handshake protocol number, world data
version, and core known-pack version.

## The floor

26.3 is the oldest release this tree speaks, and that is enforced rather than documented:

- `TERILA_MC_TARGET` below `TERILA_MC_CODE(26, 3)` is an `#error`.
- A target with no row in `version::table` fails a `static_assert`.
- At startup, `load_packet_ids` compares `_meta.protocol_version` in
  `input/packets/ids.json` with the build protocol. On a mismatch it logs the dump and build
  protocol numbers and returns `false`; `main` logs a fatal error and exits with code 7 before
  accepting connections.
- If the `_meta` object is entirely absent, the source version stays unknown (`-1`) and
  `matches_source` permits startup without a comparison. If `_meta` exists but omits
  `protocol_version`, the loader uses `-1`, treats it as a mismatch, and rejects startup.

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
