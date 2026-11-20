#!/usr/bin/env python3
"""Diff a vanilla chunk dump against Terila's dump of the same chunk.

    compare.py <input-dir> <vanilla-dump> <terila-dump> [--surface]

Prints the block match rate, which block pairs make up the mismatches, the per-height-band
breakdown, the biome match rate and, with --surface, how far the two surface heights differ.
"""

import json
import os
import sys
from collections import Counter


def load_dump(path):
    blocks = {}
    biomes = {}
    with open(path, encoding="utf-8") as file:
        for line in file:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "b":
                biomes[(int(parts[1]), int(parts[2]), int(parts[3]))] = int(parts[4])
            else:
                blocks[(int(parts[0]), int(parts[1]), int(parts[2]))] = int(parts[3])
    return blocks, biomes


def load_names(input_dir):
    with open(os.path.join(input_dir, "blocks", "state_ids.json"), encoding="utf-8") as file:
        data = json.load(file)
    names = {}
    for block in data["data"]:
        for state in block.get("states", []):
            names[state["id"]] = block["id"].replace("minecraft:", "")
    return names


def load_biome_names(input_dir):
    with open(os.path.join(input_dir, "worldgen", "biomes.json"), encoding="utf-8") as file:
        data = json.load(file)
    return {entry["protocol_id"]: entry["id"].replace("minecraft:", "") for entry in data["data"]}


def height_band(y):
    if y < 0:
        return "y < 0"
    if y < 64:
        return "y 0-63"
    if y < 128:
        return "y 64-127"
    return "y >= 128"


def summarize(input_dir, vanilla_dump, terila_dump):
    """Every number the status report shows, measured against one vanilla chunk."""
    vanilla, vanilla_biomes = load_dump(vanilla_dump)
    terila, terila_biomes = load_dump(terila_dump)
    names = load_names(input_dir)
    biome_names = load_biome_names(input_dir)

    shared = set(vanilla) & set(terila)
    matched = sum(1 for key in shared if vanilla[key] == terila[key])
    pairs = Counter()
    bands = Counter()
    band_totals = Counter()
    for key in shared:
        band = height_band(key[1])
        band_totals[band] += 1
        if vanilla[key] == terila[key]:
            continue
        bands[band] += 1
        pairs[(names.get(vanilla[key], str(vanilla[key])),
               names.get(terila[key], str(terila[key])))] += 1

    result = {
        "blocks": {
            "compared": len(shared),
            "matched": matched,
            "percent": round(100.0 * matched / len(shared), 2) if shared else 0.0,
        },
        "bands": [
            {
                "band": band,
                "wrong": bands.get(band, 0),
                "total": band_totals[band],
                "percent": round(100.0 * bands.get(band, 0) / band_totals[band], 2),
            }
            for band in sorted(band_totals)
        ],
        "pairs": [
            {"vanilla": a, "terila": b, "count": count}
            for (a, b), count in pairs.most_common(10)
        ],
    }

    biome_shared = set(vanilla_biomes) & set(terila_biomes)
    if biome_shared:
        biome_matched = sum(1 for key in biome_shared
                            if vanilla_biomes[key] == terila_biomes[key])
        biome_pairs = Counter()
        for key in biome_shared:
            if vanilla_biomes[key] != terila_biomes[key]:
                biome_pairs[(biome_names.get(vanilla_biomes[key], "?"),
                             biome_names.get(terila_biomes[key], "?"))] += 1
        result["biomes"] = {
            "compared": len(biome_shared),
            "matched": biome_matched,
            "percent": round(100.0 * biome_matched / len(biome_shared), 2),
            "pairs": [
                {"vanilla": a, "terila": b, "count": count}
                for (a, b), count in biome_pairs.most_common(5)
            ],
        }

    surface = {}
    for blocks in (vanilla, terila):
        heights = {}
        for (x, y, z), state in blocks.items():
            if names.get(state) == "air":
                continue
            column = (x, z)
            if column not in heights or y > heights[column]:
                heights[column] = y
        surface[id(blocks)] = heights
    vanilla_surface = surface[id(vanilla)]
    terila_surface = surface[id(terila)]
    diffs = Counter()
    for column in set(vanilla_surface) & set(terila_surface):
        diffs[terila_surface[column] - vanilla_surface[column]] += 1
    result["surface"] = [
        {"delta": delta, "columns": count} for delta, count in sorted(diffs.items())
    ]
    return result


def summarize_area(input_dir, vanilla_dir, terila_dir):
    """Every number the report shows, measured over a whole area of chunks.

    The dumps are matched by file name, so dumping chunk 3,-2 to `3_-2.txt` on both sides
    compares the two against each other. The aggregate is the same shape the single chunk
    report uses, plus the number of chunks and the ones furthest from vanilla.
    """
    names = load_names(input_dir)
    biome_names = load_biome_names(input_dir)

    compared = matched = 0
    biome_compared = biome_matched = 0
    bands = Counter()
    band_totals = Counter()
    pairs = Counter()
    biome_pairs = Counter()
    per_chunk = []

    for entry in sorted(os.listdir(vanilla_dir)):
        vanilla_path = os.path.join(vanilla_dir, entry)
        terila_path = os.path.join(terila_dir, entry)
        if not (os.path.isfile(vanilla_path) and os.path.isfile(terila_path)):
            continue
        vanilla, vanilla_biomes = load_dump(vanilla_path)
        terila, terila_biomes = load_dump(terila_path)

        shared = set(vanilla) & set(terila)
        chunk_matched = 0
        for key in shared:
            band = height_band(key[1])
            band_totals[band] += 1
            if vanilla[key] == terila[key]:
                chunk_matched += 1
                continue
            bands[band] += 1
            pairs[(names.get(vanilla[key], str(vanilla[key])),
                   names.get(terila[key], str(terila[key])))] += 1
        compared += len(shared)
        matched += chunk_matched

        biome_shared = set(vanilla_biomes) & set(terila_biomes)
        chunk_biomes = 0
        for key in biome_shared:
            if vanilla_biomes[key] == terila_biomes[key]:
                chunk_biomes += 1
            else:
                biome_pairs[(biome_names.get(vanilla_biomes[key], "?"),
                             biome_names.get(terila_biomes[key], "?"))] += 1
        biome_compared += len(biome_shared)
        biome_matched += chunk_biomes

        per_chunk.append({
            "chunk": entry,
            "blocks": round(100.0 * chunk_matched / len(shared), 2) if shared else 0.0,
            "biomes": round(100.0 * chunk_biomes / len(biome_shared), 2) if biome_shared else 0.0,
        })

    if not per_chunk:
        raise SystemExit("no matching chunk dumps in %s and %s" % (vanilla_dir, terila_dir))

    per_chunk.sort(key=lambda item: item["blocks"])
    result = {
        "blocks": {
            "compared": compared,
            "matched": matched,
            "percent": round(100.0 * matched / compared, 2) if compared else 0.0,
        },
        "bands": [
            {
                "band": band,
                "wrong": bands.get(band, 0),
                "total": band_totals[band],
                "percent": round(100.0 * bands.get(band, 0) / band_totals[band], 2),
            }
            for band in sorted(band_totals)
        ],
        "pairs": [
            {"vanilla": a, "terila": b, "count": count}
            for (a, b), count in pairs.most_common(10)
        ],
        "chunks": {
            "count": len(per_chunk),
            "worst": per_chunk[:5],
            "best": per_chunk[-5:],
        },
    }
    if biome_compared:
        result["biomes"] = {
            "compared": biome_compared,
            "matched": biome_matched,
            "percent": round(100.0 * biome_matched / biome_compared, 2),
            "pairs": [
                {"vanilla": a, "terila": b, "count": count}
                for (a, b), count in biome_pairs.most_common(5)
            ],
        }
    return result


def print_report(result):
    blocks = result["blocks"]
    print("blocks compared : %d" % blocks["compared"])
    print("blocks matched  : %d (%.2f%%)" % (blocks["matched"], blocks["percent"]))

    print("\nmismatches by height band:")
    for entry in result["bands"]:
        print("  %-10s %7d / %7d  (%.1f%%)" % (entry["band"], entry["wrong"],
                                                entry["total"], entry["percent"]))

    print("\ntop mismatched pairs (vanilla -> terila):")
    for entry in result["pairs"]:
        print("  %-24s -> %-24s %7d" % (entry["vanilla"], entry["terila"], entry["count"]))

    biomes = result.get("biomes")
    if biomes:
        print("\nbiomes compared : %d" % biomes["compared"])
        print("biomes matched  : %d (%.2f%%)" % (biomes["matched"], biomes["percent"]))
        for entry in biomes["pairs"]:
            print("  %-24s -> %-24s %7d" % (entry["vanilla"], entry["terila"],
                                            entry["count"]))

    print("\nsurface height delta (terila - vanilla):")
    for entry in result["surface"]:
        print("  %+4d blocks: %4d columns" % (entry["delta"], entry["columns"]))


def main(argv):
    if len(argv) < 4:
        raise SystemExit(__doc__)
    result = summarize(argv[1], argv[2], argv[3])
    print_report(result)


if __name__ == "__main__":
    main(sys.argv)
