# Changelog

All notable changes to Terila, newest first. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

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
