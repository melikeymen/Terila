# Project status

Measured by the Update Status workflow for commit `1a1e87e0` on 2026-11-25. The run extracts the game data, generates the same 9 chunks with a real 26.3 server and with this generator, and compares them block by block.

## Benchmark

| Stage | Time (seconds) |
| --- | --- |
| Extracting the game data (Minecraft 26.3) | 78 |
| Generating the area with the real server | 78 |
| Generating the area with this generator | 8 |

## World generation parity

Seed 12345, 9 chunks compared block by block.

| Measurement | Result |
| --- | --- |
| Blocks matching vanilla | 827038 / 884736 (93.48%)
| Biomes matching vanilla | 13824 / 13824 (100.00%)

### Blocks that differ, by height

| Height | Mismatched | Total | Share |
| --- | --- | --- | --- |
| y 0-63 | 27786 | 147456 | 18.84% |
| y 64-127 | 0 | 147456 | 0.00% |
| y < 0 | 29912 | 147456 | 20.29% |
| y >= 128 | 0 | 442368 | 0.00% |

### Chunks furthest from vanilla

| Chunk | Blocks matching | Biomes matching |
| --- | --- | --- |
| 1_1.txt | 91.36% | 100.00% |
| 1_0.txt | 92.33% | 100.00% |
| 1_-1.txt | 92.62% | 100.00% |
| 0_1.txt | 92.85% | 100.00% |
| 0_-1.txt | 92.86% | 100.00% |

### Most common remaining differences

| Vanilla | Terila | Blocks |
| --- | --- | --- |
| air | deepslate | 7380 |
| tuff | deepslate | 5381 |
| water | stone | 2550 |
| deepslate | air | 2048 |
| granite | stone | 1917 |
| gravel | sand | 1784 |
| stone | water | 1726 |
| andesite | stone | 1722 |
| diorite | stone | 1708 |
| moss_block | deepslate | 1670 |
