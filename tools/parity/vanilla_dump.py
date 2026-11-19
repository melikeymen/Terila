#!/usr/bin/env python3
"""Dump a vanilla chunk out of a region file in the format the parity tools compare.

    vanilla_dump.py <input-dir> <region-file> <chunk-x> <chunk-z> <out-file>

Blocks are written as "<world-x> <y> <world-z> <state-id>" and biomes as
"b <world-x> <y> <world-z> <biome-id>", using the ids from the game's own registry dump so
the numbers can be compared with what Terila's generator produces.
"""

import json
import os
import struct
import sys
import zlib


def read_nbt(buf):
    pos = 0

    def read(fmt, size):
        nonlocal pos
        value = struct.unpack_from(fmt, buf, pos)[0]
        pos += size
        return value

    def string():
        nonlocal pos
        length = read(">H", 2)
        value = buf[pos:pos + length].decode("utf-8")
        pos += length
        return value

    def payload(tag):
        nonlocal pos
        if tag == 0:
            return None
        if tag == 1:
            return read(">b", 1)
        if tag == 2:
            return read(">h", 2)
        if tag == 3:
            return read(">i", 4)
        if tag == 4:
            return read(">q", 8)
        if tag == 5:
            return read(">f", 4)
        if tag == 6:
            return read(">d", 8)
        if tag == 7:
            length = read(">i", 4)
            value = buf[pos:pos + length]
            pos += length
            return value
        if tag == 8:
            return string()
        if tag == 9:
            inner = read(">b", 1)
            length = read(">i", 4)
            return [payload(inner) for _ in range(length)]
        if tag == 10:
            value = {}
            while True:
                child = read(">b", 1)
                if child == 0:
                    return value
                name = string()
                value[name] = payload(child)
        if tag == 11:
            length = read(">i", 4)
            values = list(struct.unpack_from(">%di" % length, buf, pos))
            pos += length * 4
            return values
        if tag == 12:
            length = read(">i", 4)
            values = list(struct.unpack_from(">%dq" % length, buf, pos))
            pos += length * 8
            return values
        raise ValueError("unknown tag %d" % tag)

    tag = read(">b", 1)
    if tag != 10:
        raise ValueError("root is not a compound")
    string()
    return payload(tag)


def unpack(data, bits, count):
    if not data:
        return [0] * count
    mask = (1 << bits) - 1
    # Chunk palettes pad every long, so a value never straddles two of them. A flat bit
    # stream is kept as a fallback for files written by an older layout.
    per_long = 64 // bits
    if len(data) * per_long >= count:
        return [(data[index // per_long] >> ((index % per_long) * bits)) & mask
                for index in range(count)]
    values = []
    for index in range(count):
        bit = index * bits
        long_index = bit >> 6
        offset = bit & 63
        if long_index >= len(data):
            break
        word = data[long_index] & 0xFFFFFFFFFFFFFFFF
        value = word >> offset
        if offset + bits > 64 and long_index + 1 < len(data):
            following = data[long_index + 1] & 0xFFFFFFFFFFFFFFFF
            value |= following << (64 - offset)
        values.append(value & mask)
    values.extend([0] * (count - len(values)))
    return values


def load_state_ids(input_dir):
    with open(os.path.join(input_dir, "blocks", "state_ids.json"), encoding="utf-8") as file:
        data = json.load(file)
    ids = {}
    for block in data["data"]:
        name = block["id"]
        for state in block.get("states", []):
            properties = state.get("properties") or {}
            key = (name, tuple(sorted(properties.items())))
            ids[key] = state["id"]
        ids.setdefault((name, ()), block.get("default_state_id", -1))
    return ids


def load_biome_ids(input_dir):
    with open(os.path.join(input_dir, "worldgen", "biomes.json"), encoding="utf-8") as file:
        data = json.load(file)
    return {entry["id"]: entry["protocol_id"] for entry in data["data"]}


def region_chunk(path, chunk_x, chunk_z):
    with open(path, "rb") as file:
        header = file.read(4096)
    index = (chunk_x & 31) + (chunk_z & 31) * 32
    entry = struct.unpack_from(">I", header, index * 4)[0]
    sector = entry >> 8
    if sector == 0:
        raise SystemExit("chunk %d,%d is not in %s" % (chunk_x, chunk_z, path))
    with open(path, "rb") as file:
        file.seek(sector * 4096)
        length = struct.unpack(">I", file.read(4))[0]
        compression = file.read(1)[0]
        payload = file.read(length - 1)
    if compression == 1:
        payload = zlib.decompress(payload, 16 + zlib.MAX_WBITS)
    elif compression == 2:
        payload = zlib.decompress(payload)
    elif compression != 3:
        raise SystemExit("unsupported compression %d" % compression)
    return read_nbt(payload)


def main(argv):
    if len(argv) < 6:
        raise SystemExit(__doc__)
    input_dir, region, chunk_x, chunk_z, out_path = argv[1:6]
    chunk_x, chunk_z = int(chunk_x), int(chunk_z)

    states = load_state_ids(input_dir)
    biomes = load_biome_ids(input_dir)
    chunk = region_chunk(region, chunk_x, chunk_z)

    ox, oz = chunk_x * 16, chunk_z * 16
    lines = []
    for section in chunk.get("sections", []):
        section_y = section["Y"]
        base = section_y * 16
        block_states = section.get("block_states") or {}
        palette = block_states.get("palette") or [{"Name": "minecraft:air"}]
        resolved = []
        for entry in palette:
            if isinstance(entry, str):
                name = entry
                properties = ()
            elif "" in entry and len(entry) == 1:
                name = entry[""]
                properties = ()
            else:
                name = entry.get("Name", "minecraft:air")
                properties = tuple(sorted((entry.get("Properties") or {}).items()))
            if (name, properties) in states:
                resolved.append(states[(name, properties)])
            elif (name, ()) in states:
                resolved.append(states[(name, ())])
            else:
                resolved.append(-1)
        data = block_states.get("data")
        if not data:
            indices = [0] * 4096
        else:
            bits = max(4, (len(palette) - 1).bit_length())
            indices = unpack(data, bits, 4096)
        for index, entry in enumerate(indices):
            y = base + (index >> 8)
            z = (index >> 4) & 15
            x = index & 15
            lines.append("%d %d %d %d" % (ox + x, y, oz + z, resolved[entry]))

        biome_data = section.get("biomes") or {}
        biome_palette = biome_data.get("palette") or ["minecraft:plains"]
        biome_ids = [biomes.get(name, -1) for name in biome_palette]
        biome_long = biome_data.get("data")
        if not biome_long:
            biome_indices = [0] * 64
        else:
            bits = max(1, (len(biome_palette) - 1).bit_length())
            biome_indices = unpack(biome_long, bits, 64)
        for index, entry in enumerate(biome_indices):
            y = base + ((index >> 4) << 2) + 2
            z = ((index >> 2) & 3) * 4 + 2
            x = (index & 3) * 4 + 2
            lines.append("b %d %d %d %d" % (ox + x, y, oz + z, biome_ids[entry]))

    with open(out_path, "w", encoding="utf-8") as file:
        file.write("\n".join(lines))
        file.write("\n")

    print("wrote %d lines from %s" % (len(lines), region))


if __name__ == "__main__":
    main(sys.argv)
