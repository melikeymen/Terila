# Project status

Measured by the Update Status workflow for commit `cf005649` on 2026-11-25. The run extracts the game data, generates the same 9 chunks with a real 26.3 server and with this generator, and compares them block by block.

## Benchmark

| Stage | Time (seconds) |
| --- | --- |
| Extracting the game data (Minecraft 26.3) | 82 |
| Generating the area with the real server | 78 |
| Generating the area with this generator | 12 |

## World generation parity

Seed 12345, 9 chunks compared block by block.

| Measurement | Result |
| --- | --- |
| Blocks matching vanilla | 850867 / 884736 (96.17%)
| Biomes matching vanilla | 13824 / 13824 (100.00%)

### Blocks that differ, by height

| Height | Mismatched | Total | Share |
| --- | --- | --- | --- |
| y 0-63 | 17052 | 147456 | 11.56% |
| y 64-127 | 0 | 147456 | 0.00% |
| y < 0 | 16817 | 147456 | 11.40% |
| y >= 128 | 0 | 442368 | 0.00% |

### Chunks furthest from vanilla

| Chunk | Blocks matching | Biomes matching |
| --- | --- | --- |
| 0_-1.txt | 95.01% | 100.00% |
| 0_1.txt | 95.36% | 100.00% |
| 1_-1.txt | 95.74% | 100.00% |
| 1_0.txt | 95.89% | 100.00% |
| 1_1.txt | 96.05% | 100.00% |

### Most common remaining differences

| Vanilla | Terila | Blocks |
| --- | --- | --- |
| air | deepslate | 1987 |
| gravel | sand | 1854 |
| tuff | deepslate | 1630 |
| moss_block | deepslate | 1622 |
| stone | deepslate | 1600 |
| deepslate | stone | 1550 |
| deepslate | bedrock | 1548 |
| bedrock | deepslate | 1524 |
| water | kelp_plant | 1395 |
| water | stone | 982 |
