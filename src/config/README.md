# terila::config

Server configuration for Terila. Two headers, no translation unit: the parser
is header-only so the server can read its settings before anything else is
initialised.

- `server_config.hpp` — the `ServerConfig` struct and the `server.properties`
  reader.
- `server_files.hpp` — the default `server.properties` and `eula.txt` contents,
  plus the helpers that create them on first run.

## ServerConfig

Every field has a default that matches the file written on first run, so a
missing key never changes behaviour.

| Field                 | Type            | Default     | Meaning                                                       |
| --------------------- | --------------- | ----------- | ------------------------------------------------------------- |
| `bind_address`        | `std::string`   | `"0.0.0.0"` | Address the listener binds to                                 |
| `port`                | `std::uint16_t` | `25565`     | TCP port; out-of-range values keep the old one                |
| `log_level`           | `std::string`   | `"info"`    | Passed to `log::parse_level`                                  |
| `log_file`            | `std::string`   | `""`        | Empty means console only                                      |
| `log_exit_level`      | `std::string`   | `"fatal"`   | Passed to `log::parse_level`; `never` disables the exit path  |
| `level_seed`          | `std::string`   | `"terila"`  | Numeric text is used as a seed, anything else is hashed       |
| `level_type`          | `std::string`   | `"normal"`  | `normal` or `flat`; unknown values fall back to normal        |
| `online_mode`         | `bool`          | `false`     | `false` lets offline (cracked) clients join                   |
| `network_encryption`  | `bool`          | `false`     | AES/CFB8 after the RSA exchange, independent of `online_mode` |
| `view_distance`       | `int`           | `10`        | Chunk radius streamed around the player, clamped to `[2, 32]` |
| `simulation_distance` | `int`           | `10`        | Ticking radius, clamped to `[1, 32]`                          |
| `max_players`         | `int`           | `20`        | Advertised in login and status, clamped to `[1, 1000]`        |
| `gamemode`            | `int`           | `1`         | `0` survival, `1` creative, `2` adventure, `3` spectator      |

### Seed handling

`seed_value()` returns a `std::uint32_t`:

1. If `level_seed` parses as a signed 64-bit integer, the low 32 bits are used
   unchanged, so `level-seed=-12345` and `level-seed=4294954951` produce the
   same world.
2. Otherwise the text is hashed with FNV-1a (offset `2166136261`, prime
   `16777619`) over its bytes, so the same text always produces the same seed
   on every platform.

The hash is deliberately not `std::hash<std::string>`: that is allowed to
differ between standard library versions and would silently change a world.

## Reading `server.properties`

```cpp
terila::config::ServerConfig cfg;
if (!terila::config::load(cfg, "server.properties")) {
    // file missing or unreadable; cfg still holds the defaults
}
```
