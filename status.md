# Project status

Measured by the Update Status workflow for commit `8f0f9f88` on 2026-11-25. The run extracts the game data, generates the same 9 chunks with a real 26.3 server and with this generator, and compares them block by block.

## Benchmark

| Stage | Time (seconds) |
| --- | --- |
| Extracting the game data (Minecraft 26.3) | 81 |
| Generating the area with the real server | 78 |
| Generating the area with this generator | 12 |

## World generation parity

Seed 12345, 9 chunks compared block by block.

| Measurement | Result |
| --- | --- |
| Blocks matching vanilla | 856860 / 884736 (96.85%)
| Biomes matching vanilla | 13824 / 13824 (100.00%)

### Blocks that differ, by height

| Height | Mismatched | Total | Share |
| --- | --- | --- | --- |
| y 0-63 | 14942 | 147456 | 10.13% |
| y 64-127 | 0 | 147456 | 0.00% |
| y < 0 | 12934 | 147456 | 8.77% |
| y >= 128 | 0 | 442368 | 0.00% |

### Chunks furthest from vanilla

| Chunk | Blocks matching | Biomes matching |
| --- | --- | --- |
| 0_-1.txt | 95.65% | 100.00% |
| 0_1.txt | 96.35% | 100.00% |
| 1_-1.txt | 96.43% | 100.00% |
| 1_0.txt | 96.48% | 100.00% |
| -1_0.txt | 96.89% | 100.00% |

### Most common remaining differences

| Vanilla | Terila | Blocks |
| --- | --- | --- |
| gravel | sand | 1854 |
| air | deepslate | 1713 |
| tuff | deepslate | 1631 |
| moss_block | deepslate | 1582 |
| water | kelp_plant | 1396 |
| water | stone | 1100 |
| water | air | 987 |
| deepslate | air | 813 |
| water | deepslate | 801 |
| clay | deepslate | 624 |
