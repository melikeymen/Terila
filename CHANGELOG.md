# Changelog

All notable changes to Terila, newest first. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- The plugin API's first slice: `plugins/` is scanned at start, and a folder counts as a
  plugin only when it carries both `plugin.json` and `permissions.json`. Ids, capability
  names and the module path are validated, and the folder must be named after the id
- A plugin runs only when the operator's own `config/plugin-grants/<id>.json` grants some of
  what it asked for. What it holds is `requested ∩ granted`, never the request, and a
  package can never grant itself anything
- The WebAssembly runtime is found by name at run time, like the OpenCL device, so the server
  builds and starts without one; a build without the Wasmtime C API header still enforces the
  gate and says in the log why nothing was loaded. Every refusal is one line under the new
  PLUGIN scope
- The first host function of ABI version 1: `terila.log_write` is linked only when the
  plugin actually holds `log.write`, so a module that imports more than it was granted fails
  to instantiate and the refusal names the missing import. Text is copied out of guest memory
  with a size and range check, folded to one line, and logged under the plugin's id
- The grant's `limits` are read: `fuel_per_call` (default 200000) bounds every call into a
  plugin, and `events_per_second` (default 100) throttles `log.write`, which drops the extra
  lines and hands the guest the documented `-2`. A grant without `limits` gets the defaults,
  and a value outside the ceiling is a refusal, not a silent clamp
- A module the runtime cannot compile now reports the runtime's own reason (opcode and
  offset) instead of only "not valid WebAssembly".
- `player.read` adds `terila.player_count()` and
  `terila.player_read(pointer, capacity, index)`: the host writes one fixed 64-byte record
  (entity id, 32-byte NUL-padded name, dimension, x, y, z) into the guest's own memory and
  answers with its size. A bad index is -1, a capacity below one record is -4, a range
  outside guest memory is -5. The server supplies the list through `set_player_source`, in
  ascending entity id order, so an index stays valid across joins and leaves.
- The second host function, `terila.chat_send(pointer, length, target)`, linked only for
  `chat.send`: target 0 reaches every joined player and a positive id reaches one of them,
  through the server's own system-chat packet. A target that names no online player is a bad
  argument (-1), not a delivery, and a call with no sink answers "unavailable" (-3) instead
  of failing. Both host functions draw from the plugin's one `events_per_second` budget
- Trap policy: a trapping call is dropped and counted, and after 32 traps the instance is
  released and the plugin is listed as disabled with the reason, so a broken plugin cannot
  keep failing on every event for the rest of the run
- `plugin_tests` covers the gate without a runtime: unknown capabilities, duplicate requests,
  module paths that escape the package, missing grants, and folder/id mismatch. With a
  runtime present it also drives real modules: an accepted `on_load`, a refused one that
  leaves no store or reachable instance, and the `log.write` import on both sides of the gate
- `configs/terila.config` carries `[worldgen] backend=cpu|gpu|auto` (cpu unless it is
  changed): the density field of every chunk can be evaluated on an OpenCL device
- The chunk grid and a single block read blend the eight corners of a density cell through
  one shared function, so the interpolation cannot drift between the streamed chunk and the
  block an edit reads
- The device path walks the same density graph as the chunk workers, node by node, on an
  explicit stack, and every pool, template and noise table travels to the device as plain
  arrays. A graph with a node the kernel does not carry keeps the CPU path
- The device is opened through the system OpenCL loader at runtime, so no build dependency
  and no SDK are needed; a machine without a device that computes in doubles keeps the CPU
  field, and `backend=gpu` says why in the log
- Round-trip tests build every branch of the tree and whole chunks on both sides and compare
  them: the density values must match bit for bit, and so must the block array of a chunk
- `backend=auto` is a measurement instead of a guess: the first generator samples one chunk
  of field through both paths and the faster side keeps the work for the rest of the process
- The device binds its buffers once per graph instead of once per chunk, hands its tables
  back when the last generator that holds them goes away, and parks itself after a batch it
  cannot complete instead of retrying every chunk

### Changed

- A player who disconnects stops the chunk jobs still queued for him instead of leaving the
  workers generating a whole view distance nobody will stream, so the next player's columns
  no longer wait behind the queue of the player who left
- Measured on an RTX 3050 Ti: one chunk's density field takes 2.35 ms on the device against
  roughly 28 ms on a worker, which makes a chunk 85.0 → 59.4 ms with one generator and
  24.6 → 18.1 ms with eight (`TERILA_BENCH=1 chunk_tests --gtest_filter='TerrainGeneratorGpuBench.*'`)
- The benchmark also times sixteen chunks in a single dispatch: 3.8 ms/chunk one after
  another against 0.4 ms/chunk together, so the device's per-dispatch cost is most of what
  a single chunk pays. Batching the chunk workers' requests was measured and does not pay
  at eight workers, where the device is busy about an eighth of the time

### Fixed

- A chunk worker that generates two dimensions no longer answers one of them with the
  other's interpolated lattice points: the per-thread lattice cache is cleared when the
  world it was filled for changes
- `worldgen_dump` links again: the parity tool needs the device backend unit as well, which
  the Linux build of the tool was missing
- The shared world generator no longer holds the connection that happened to initialise a
  dimension: the block, biome and template-state resolvers it keeps for its chunk workers
  now capture the block registry instead of the connection, so a player logging out while
  those workers are placing structures no longer leaves them reading a destroyed object
- The device binds the kernel arguments whenever the density graph changes, so an overworld
  and a nether generator alternating on the same kernel no longer evaluate one dimension's
  field with the other's tables, and the device object is never destroyed while a plan can
  still hand its tables back
- A joining player receives the chunks around the spawn instead of an empty world: the
  template pools, templates, processor lists and jigsaw candidate lists that generation
  fills in are now shared between the chunk workers safely, so a worker no longer stops
  while another one was writing the same cache
- A chunk stream that dies because one of its chunks failed is logged and the batch is
  closed instead of leaving the client waiting on a batch that never finishes

## [v0.23.0] — 2026-11-22

### Added

- The overworld, the nether and the end each keep their own generator, chunk cache and
  region files, so a column saved in one dimension is never served in another
- `execute in <dimension> run tp` moves a player between the dimensions, streaming the new
  world and sending a respawn packet with that dimension's type, name and sea level
- Flint and steel and the fire charge open an obsidian frame into a nether portal: the
  opening is 2..21 blocks wide and 3..21 tall, and lighting any cell inside it fills the
  opening with portal blocks of the right axis
- A frame that is not complete leaves a fire on the block that was used, like the item
  does in the open
- The login and respawn packets carry the dimension type the client was handed during
  configuration, instead of always claiming the overworld
- `configs/terila.config` holds the runtime resource limits: the memory ceiling, the memory
  kept free for the machine, and the floor and ceiling of the chunk worker pool. The
  directory and the file are created on the first start and an existing file is never
  rewritten
- The chunk generation pool takes its worker count from that config, and each dimension's
  chunk cache is sized from its memory budget instead of a fixed 96 columns

### Changed

- The block overlay is keyed by dimension: the nether and the overworld share their x/z, so
  a single table put one world's edit on top of the other's column
- The light engine drains its flood fill from a flat queue it reads forward instead of a
  deque it pops, which takes about a fifth of the chunk's lighting time off the generator
- A connection writes the first few packets waiting in its queue as one gathered write
  instead of one write per packet
- Players only see each other inside one dimension, both as entities and through selectors

### Fixed

- A village, outpost or any other assembled structure sits on the ground under its own
  pieces instead of on one height taken at its centre, and the gap a piece leaves over a
  slope is filled, so no building hangs in the air
- A ruined portal, a shipwreck or any other single-template structure fills the gap under
  its lowest layer on a slope, so it no longer hangs over the drop
- A `rollable` jigsaw joint is joined like an aligned one, so the pieces a village hangs
  under its meeting point (the well, its villagers, its cats) are placed instead of dropped
- A whole template is laid down with one of the four quarter turns picked from its site, so
  a wreck or a portal is not always built the same way round; the stairs and rails of the
  turned template turn with it
- A village, outpost or city grows as deep as the structure's `size` and no farther from
  its centre than `max_distance_from_center`, both read from the structure data instead of
  a fixed cap
- A jigsaw piece is laid down with the quarter turn that faces the piece it joins, and its
  stairs, doors, pillars and signs are turned with it, so a house joins a street whichever
  way the street runs
- A jigsaw connection reads its orientation from the right offset of the block state, so a
  piece must actually face the connection instead of being accepted whatever way it faced
- A connection that drew a piece which does not fit tries the rest of the pool before it is
  given up, instead of dropping the street
- Respawn after a death returns the player to the dimension and spawn they died in
- A save queued before a dimension change is written to the region files of the dimension
  it was generated in, not to the new one's
- A chunk batch left in flight by a dimension change is dropped instead of streaming the
  old world's columns over the new one
- Terrain stopped streaming partway through the view distance: a stream that had already
  taken its batch out of the queue was only resumed when the queue was non-empty, so the
  last chunks of a login were never sent and the client stayed on "Loading terrain"
- The stream is also pumped by the server tick, so a batch waiting on a worker while the
  socket is quiet still finishes instead of waiting for a write that never comes
- A column queued for saving in one dimension is no longer handed to a player asking for
  the same coordinate in another
- The block and biome ids and the generator are resolved once per dimension instead of
  once per streamed chunk
- A finished chunk task wakes its stream on the io thread at once instead of the stream
  polling its future once a tick, which alone tripled the streamed chunk rate
- The natural spawner runs ten times a second and stays out of the way while a player is
  still receiving the initial view distance, so join time is not spent on mob sampling
- A connection disables Nagle, which was holding packets back for the peer's delayed ack
- The listener no longer sets `SO_REUSEADDR` on Windows, where it let a second server bind
  the same port and answer alongside the first
- The chunk pool's default ceiling is sixteen workers, so a machine with more than a few
  cores generates its view distance in parallel
- A dimension change and a death respawn now send the game event that starts chunk loading;
  without it the client stayed on "Loading terrain" for its full thirty second timeout even
  though the new world was already streaming

## [v0.22.0] — 2026-11-21

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
- Nether and End dimensions: dimension types and their noise settings are read from the dump,
  so each dimension builds with its own height, sea level, fluid and skylight
- The nether floods everything under its lava sea, not only the columns below the sea level
- Nether and End biomes are resolved from their own parameter sets, the End from its island
  noise around the central island

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
