# Project status

Measured by the Update Status workflow for commit `08663b80` on 2026-11-25. The run extracts the game data, generates the same 9 chunks with a real 26.3 server and with this generator, and compares them block by block.

## Benchmark

| Stage | Time (seconds) |
| --- | --- |
| Extracting the game data (Minecraft 26.3) | 62 |
| Generating the area with the real server | 75 |
| Generating the area with this generator | 7 |

## World generation parity

Seed 12345, 9 chunks compared block by block.

| Measurement | Result |
| --- | --- |
| Blocks matching vanilla | 838952 / 884736 (94.83%)
| Biomes matching vanilla | 13824 / 13824 (100.00%)

### Blocks that differ, by height

| Height | Mismatched | Total | Share |
| --- | --- | --- | --- |
| y 0-63 | 23812 | 147456 | 16.15% |
| y 64-127 | 0 | 147456 | 0.00% |
| y < 0 | 21972 | 147456 | 14.90% |
| y >= 128 | 0 | 442368 | 0.00% |

### Chunks furthest from vanilla

| Chunk | Blocks matching | Biomes matching |
| --- | --- | --- |
| 0_-1.txt | 93.24% | 100.00% |
| 1_-1.txt | 93.74% | 100.00% |
| 0_1.txt | 94.30% | 100.00% |
| 1_0.txt | 94.56% | 100.00% |
| 1_1.txt | 94.61% | 100.00% |

### Most common remaining differences

| Vanilla | Terila | Blocks |
| --- | --- | --- |
| tuff | deepslate | 5467 |
| air | deepslate | 2074 |
| granite | stone | 1927 |
| andesite | stone | 1905 |
| gravel | sand | 1854 |
| diorite | stone | 1740 |
| moss_block | deepslate | 1703 |
| clay | deepslate | 1621 |
| bedrock | deepslate | 1602 |
| stone | deepslate | 1602 |
