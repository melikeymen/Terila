# Changelog

All notable changes to Terila, newest first. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Jigsaw structures: template pools (`template_pools.json`) and jigsaw block entities are
  read from the vanilla dump, and pieces are joined by name/orientation into a whole
  structure, capped like the game's `size` field
- Pillager outposts, all five village types and ancient cities are built from their pools,
  next to the single-template ruined portals and shipwrecks
- Processor lists: block rot, rule replacements and protected blocks weather a piece
  (cracked stone brick, mossy cobble, missing blocks) before it is written
- Structures stay inside the biomes their tag names, and shipwrecks land on the sea floor
  or the shore
- Templates carry the exact block states their palette names, not just the default state
- Jigsaw and processor test suites

### Fixed

- Template caches are keyed by the mounted input root, so a test that points the store at a
  different directory no longer poisons the real template table
- The structure palette resolver is captured by value instead of a reference to a caller
  frame that is gone by generation time

## [v0.21.0] — 2026-11-21

### Added

- Structure templates load from the vanilla dump, with their palettes resolved to states
- Structure sites are placed on the spread grid and each template is written per chunk
- Ruined portals built from their templates
- The deep filler gets vanilla stone blobs, and stone fades into deepslate above y zero
- Parity is measured over an area of chunks with the status tool

### Changed

- Generator split into density, surface and stage headers

### Fixed

- Placed light sources emit light again, and halo cells stay shared

## [v0.20.0] — 2026-11-20

### Added

- Mobs spawn on a chunk cycle, and the world carries a difficulty level
- Mob AI covers more of the vanilla goal registrations

### Changed

- Mobs are looked up by id instead of scanning the whole list
- A mob's attack fallback and undead flag are derived at spawn
- The spawn area and biome scan are no longer rebuilt every tick
- The light halo's sky tops are built once per chunk instead of per column
- Streamed chunks are no longer relit and rewritten

### Fixed

- `wait()` continues from the child that suspended, and the coroutine that actually
  suspended is resumed
- Idle despawn timers reset when a player comes close
- The per-chunk difficulty time no longer grows without bound

## [v0.19.0] — 2026-11-20

### Added

- Command system rebuilt on a registry, with `/tp`, `/summon`, `/msg` and `/gamemode`

## [v0.18.2] — 2026-11-19

### Added

- Vanilla parity harness

### Fixed

- The sea level water surface matches vanilla

### Testing

- Terrain generator tests skip when the extracted input data is missing

## [v0.18.1] — 2026-11-19

### Changed

- Command handling moved to `src/commands`, with shared selectors and `/time` in the tree

## [v0.18.0] — 2026-11-19

### Added

- Water and lava flow, with buckets and fluid mixing

## [v0.17.0] — 2026-11-19

### Added

- Terrain driven by the game's noise, climate and material data
- Sand, gravel and concrete powder fall like vanilla
- LZ4 compression in region files
- Roadmap, security policy and public release documentation

### Fixed

- `ChunkStore` is thread-safe with shared ownership and a cache mutex
- Version macros, table fields and protocol checks documented

## [v0.16.0] — 2026-11-17

### Added

- Mob AI driven by the game's data, with player health, death, respawn and a day cycle
- `/execute` runs a command as another player
- Light engine caches halo block samples

### Fixed

- Regions stay sparse on Windows (`SetEndOfFile` replaces `_chsize_s`)
- A world keeps the seed it was created with
- Edited chunks keep their lighting, so dug blocks are no longer black

### Testing

- TRF region read and write benchmark

## [v0.15.1] — 2026-11-17

### Fixed

- Edits, dropped items and block placements stay in sync with the client

## [v0.15.0] — 2026-11-16

### Added

- Block breaking takes the data-driven hardness and tool into account

### Fixed

- Every world carries its own identity; region seals stay verifiable

## [v0.14.3] — 2026-11-16

### Added

- Minecraft protocol chosen at build time, differences behind feature flags

### Changed

- Generated chunks are shared instead of copied on the packet path

## [v0.14.2] — 2026-11-16

### Fixed

- `/give` accepts a target selector before the item

## [v0.14.1] — 2026-11-16

### Changed

- NBT tree layer renamed to `nbt`, chunks stream via coroutines

## [v0.14.0] — 2026-11-15

### Changed

- Unified `TagValue` tree replaces the old `NbtValue` system

## [v0.13.1] — 2026-11-15

### Added

- Players identified by their real account, skins sent to clients
- Items given by SNBT component

### Fixed

- Chunks lit like vanilla

## [v0.13.0] — 2026-11-15

### Added

- Command tree sent to the client
- Fresh install no longer dropped

## [v0.12.4] — 2026-11-14

### Fixed

- System chat uses Classic NBT for network NBT

## [v0.12.3] — 2026-11-14

### Changed

- All NBT callers migrated to `Writer`; inline helpers removed

## [v0.12.2] — 2026-11-14

### Added

- Network and Classic NBT reader and writer

## [v0.12.1] — 2026-11-14

### Fixed

- Chunk decoder unbounded allocation and SIGFPE
- Fuzz regression corpus and sanitizer presets added

## [v0.12.0] — 2026-11-14

### Fixed

- Spawn on land near sea level
- Mobs stand on the terrain

## [v0.11.0] — 2026-11-14

### Added

- Data component item stacks
- Slash commands
- Selectable world type
- Real terrain with biomes generated from a world seed
- Crafting table and furnace menus with recipes

## [v0.10.1] — 2026-11-13

### Added

- Collected drops go to the player inventory
- Every gamemode supported
- Broken blocks drop as items, picked up with the mouse

## [v0.10.0] — 2026-11-13

### Added

- Terila Region Format: worlds persist in `.trf` region files
- Players see each other; player count is real

### Fixed

- CRC32C intrinsics compiled for the `sse4.2` target

## [v0.9.0] — 2026-11-12

### Added

- Players hit mobs
- Every living mob type spawns with AI

### Fixed

- Serverbound configuration ids resolved; connection closes on write failures

## [v0.8.0] — 2026-11-11

### Added

- Wandering mobs spawn and their movement streams to clients

## [v0.7.0] — 2026-11-11

### Added

- Chunks stream around the player by view distance

## [v0.6.0] and earlier

Not yet documented. See `git log` for the history.
