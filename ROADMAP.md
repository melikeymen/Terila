# Roadmap

Terila is a clean-room Minecraft 26.3 (protocol 777) server. This is what
is planned, roughly in order. Nothing here is a promise.

## Now

### Gameplay
- [ ] Mob AI: pathfinding, goals, breeding, combat
- [ ] Physics: falling blocks, fluid flow
- [ ] Boats and minecarts
- [ ] Full command set: `/tp`, `/gamemode`, `/time`, `/weather`, `/kill`,
      `/summon`, `/effect`, `/enchant`, `/xp`, `/setblock`, `/fill`, `/clone`

### World
- [ ] Player persistence across restarts (inventory, position, health, XP)
- [ ] World border
- [ ] Scoreboard and teams

### Stability
- [ ] Stress test: 100 bots, 20 TPS, 1 hour

## Next

### Gameplay
- [ ] Villager trading and AI
- [ ] Advancements
- [ ] Redstone

### Extensibility
- [ ] Plugin API (LGPL, so plugins can stay closed-source)
- [ ] Event system: block break, player join, chat, entity spawn
- [ ] Datapack loading: recipes, loot tables, tags from the extractor

### World
- [ ] Multi-world support (Nether, End, custom dimensions)
- [ ] Structure generation from templates
- [ ] Entity persistence

## Later

### Scale
- [ ] 200+ players on one server
- [ ] Async chunk I/O

### Extras
- [ ] Custom dimensions from datapacks
- [ ] Permissions: op levels, per-command
- [ ] Chat: signed messages, chat reports

## Not planned

- Bedrock protocol
- Anvil world import (the extractor produces data, not worlds)
- Java Edition client modifications
- Forge/Fabric mod compatibility (plugins are the extension path)