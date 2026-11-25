# Project status

Measured by the Update Status workflow for commit `29b205d3` on 2026-11-25. The run extracts the game data, generates the same 9 chunks with a real 26.3 server and with this generator, and compares them block by block.

## Benchmark

| Stage | Time (seconds) |
| --- | --- |
| Extracting the game data (Minecraft 26.3) | 68 |
| Generating the area with the real server | 75 |
| Generating the area with this generator | 6 |

## World generation parity

Seed 12345, 9 chunks compared block by block.

| Measurement | Result |
| --- | --- |
| Blocks matching vanilla | 838905 / 884736 (94.82%)
| Biomes matching vanilla | 13824 / 13824 (100.00%)

### Blocks that differ, by height

| Height | Mismatched | Total | Share |
| --- | --- | --- | --- |
| y 0-63 | 23808 | 147456 | 16.15% |
| y 64-127 | 0 | 147456 | 0.00% |
| y < 0 | 22023 | 147456 | 14.94% |
| y >= 128 | 0 | 442368 | 0.00% |

### Chunks furthest from vanilla

| Chunk | Blocks matching | Biomes matching |
| --- | --- | --- |
| 0_-1.txt | 93.24% | 100.00% |
| 1_-1.txt | 93.72% | 100.00% |
| 0_1.txt | 94.30% | 100.00% |
| 1_0.txt | 94.54% | 100.00% |
| 1_1.txt | 94.62% | 100.00% |

### Most common remaining differences

| Vanilla | Terila | Blocks |
| --- | --- | --- |
| tuff | deepslate | 5464 |
| air | deepslate | 2070 |
| granite | stone | 1927 |
| andesite | stone | 1905 |
| gravel | sand | 1854 |
| diorite | stone | 1740 |
| moss_block | deepslate | 1716 |
| clay | deepslate | 1655 |
| bedrock | deepslate | 1602 |
| stone | deepslate | 1602 |
