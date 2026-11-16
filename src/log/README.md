# terila::log

The logging layer for Terila. One header pair (`logger.hpp`, `level.hpp`) and one
translation unit (`logger.cpp`); no external dependencies beyond the standard
library and `std::format`.

## Levels

Seven levels, ordered from least to most severe. The numeric values are fixed so
that `is_enabled` can compare with `<`.

| Level      | Value | Meaning                                                              | Process effect           |
| ---------- | ----- | -------------------------------------------------------------------- | ------------------------ |
| `Trace`    | 0     | Step-by-step flow: per-packet, per-chunk, per-loop values            | None                     |
| `Debug`    | 1     | Developer diagnostics: handshake, state transitions, decisions       | None                     |
| `Info`     | 2     | Expected milestones: server started, player joined, world ready      | None                     |
| `Warn`     | 3     | Tolerable problems: retry, fallback, budget overrun, unanswered ping | Continues                |
| `Error`    | 4     | An operation failed; the process is still healthy                    | Continues                |
| `Critical` | 5     | A subsystem failed; the process runs in a degraded mode              | Continues                |
| `Fatal`    | 6     | The process cannot continue safely                                   | `std::exit(7)` (default) |

`Error`, `Critical` and `Fatal` are not three shades of "worse": they describe
three different outcomes.

- `Error` — the operation failed and the caller handles it. Nothing about the
  process changes.
- `Critical` — a subsystem is down but the process keeps running in a reduced
  form. For example, a TRF write is aborted and the region is buffered in
  memory until the next attempt. The caller decides whether the reduced form is
  acceptable; the logger only records that it happened.
- `Fatal` — the process has no safe state left. A varint decode failure is
  fatal because every packet depends on it; a TRF out-of-memory after the
  in-memory fallback is fatal because there is nowhere left to buffer.

## Scopes

A fixed set of subsystem tags, so log lines can be filtered per module. The bit
position equals the enum value, and `Config::scope_mask` turns each one on or
off.

| Scope      | Value | Covers                                        |
| ---------- | ----- | --------------------------------------------- |
| `Main`     | 0     | Startup, shutdown, world directory            |
| `Network`  | 1     | Connection, handshake, keep-alive             |
| `Protocol` | 2     | Packet encode/decode, varint, strings         |
| `Data`     | 3     | Block/registry data, tag payload budgets      |
| `Registry` | 4     | Datapack registries, known packs              |
| `Config`   | 5     | Server configuration                          |
| `Play`     | 6     | Gameplay: movement, block break/place, attack |
| `Chunk`    | 7     | Chunk streaming, batch, template              |
| `Entity`   | 8     | Entity add/remove, hurt, movement             |

Default mask is `0xFFFFFFFF` (every scope enabled).

## Usage

The `LOG_*` macros are the only entry point. They check `is_enabled` first, so a
disabled call formats nothing.

```cpp
#include "log/logger.hpp"

LOG_INFO(Network, "{} joined the game", username_);
LOG_WARN(Data, "tag payload over budget: {} + {} > {} bytes", spent_, bytes, allowance_);
LOG_CRITICAL(Chunk, "trf save ({}, {}) failed: {} ({})", cx, cz, code, detail);
LOG_FATAL(Main, "cannot prepare the world directory '{}'", path);
```
