# Fuzz regressions

Every input a fuzzer found a crash with lives here as `<name>.bin`, and
`tests/fuzz/regression_tests.cpp` replays all of them on every `trf_tests` run.

## Saved inputs

| File | Found | Symptom | Cause |
| --- | --- | --- | --- |
| `chunk-palette-width.bin` | 2026-11-14 | `SIGFPE` (divide by zero) | Palette bit width 200 made `64 / bits` zero before the rejection check |
| `chunk-heightmap-count.bin` | 2026-11-14 | `std::bad_alloc` out of `decode()` | Heightmap count reached `reserve()` with no bound; 12 bytes asked for gigabytes |

Both were fixed in `src/world/chunk_decoder.hpp` by bounding the bit width (`> 32` is
rejected) and by checking counts against the bytes that remain.

## Adding a finding

1. Copy the artifact (`crash-*`, `oom-*`, `timeout-*`) from
   `out\fuzz-corpus\<target>\crashes` here with a descriptive name.
2. Add a row to the table above: date, symptom and the function that broke.
3. If the input is small enough to be worth spelling out, also add a named test next to
   the replay test so the invariant, not only the bytes, is pinned.
