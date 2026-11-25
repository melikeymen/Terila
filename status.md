# Project status

Measured by the Update Status workflow for commit `c3598624` on 2026-11-25. The run extracts the game data, generates the same 9 chunks with a real 26.3 server and with this generator, and compares them block by block.

## Benchmark

| Stage | Time (seconds) |
| --- | --- |
| Extracting the game data (Minecraft 26.3) | 69 |
| Generating the area with the real server | 76 |
| Generating the area with this generator | 6 |

## World generation parity

Seed 12345, 9 chunks compared block by block.

| Measurement | Result |
| --- | --- |
| Blocks matching vanilla | 838999 / 884736 (94.83%)
| Biomes matching vanilla | 13824 / 13824 (100.00%)

### Blocks that differ, by height

| Height | Mismatched | Total | Share |
| --- | --- | --- | --- |
| y 0-63 | 23762 | 147456 | 16.11% |
| y 64-127 | 0 | 147456 | 0.00% |
| y < 0 | 21975 | 147456 | 14.90% |
| y >= 128 | 0 | 442368 | 0.00% |

### Chunks furthest from vanilla

| Chunk | Blocks matching | Biomes matching |
| --- | --- | --- |
| 0_-1.txt | 93.24% | 100.00% |
| 1_-1.txt | 93.74% | 100.00% |
| 0_1.txt | 94.30% | 100.00% |
| 1_0.txt | 94.56% | 100.00% |
| 1_1.txt | 94.67% | 100.00% |

### Most common remaining differences

| Vanilla | Terila | Blocks |
| --- | --- | --- |
| tuff | deepslate | 5468 |
| air | deepslate | 2073 |
| granite | stone | 1919 |
| andesite | stone | 1913 |
| gravel | sand | 1854 |
| diorite | stone | 1740 |
| moss_block | deepslate | 1702 |
| clay | deepslate | 1621 |
| bedrock | deepslate | 1602 |
| stone | deepslate | 1602 |
