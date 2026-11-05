#!/usr/bin/env python3
"""Builds public/gen3/{species.json,icons.png} from a pret/pokeemerald checkout.

  python3 tools/build_gen3_data.py /path/to/pokeemerald

species.json maps Gen 3 internal species ids (what a save stores) to {name, dex, hoenn-order is
irrelevant}; icons.png is a 16-column atlas of the 32x32 party/box icons (first animation frame)
with cell number == internal species id.
"""
import json, re, sys
from pathlib import Path
from PIL import Image

root = Path(sys.argv[1])
out = Path(__file__).resolve().parent.parent / "public" / "gen3"
out.mkdir(parents=True, exist_ok=True)
read = lambda p: (root / p).read_text(encoding="utf-8", errors="replace")

# species ids (only literal ones; Unown B-Z and friends sit above the Egg and are not stored as species)
ids = {}
for m in re.finditer(r"#define\s+SPECIES_(\w+)\s+(\d+)\b", read("include/constants/species.h")):
    ids[m.group(1)] = int(m.group(2))

# national dex numbers
enum_src = re.search(r"enum\s*\{(.*?)\};", read("include/constants/pokedex.h"), re.S).group(1)
enum_src = re.sub(r"//.*", "", enum_src)
dex = {}
for i, name in enumerate(n.strip() for n in enum_src.split(",") if n.strip()):
    if name.startswith("NATIONAL_DEX_"):
        dex[name[len("NATIONAL_DEX_"):]] = i
to_dex = {}
for m in re.finditer(r"SPECIES_TO_NATIONAL\((\w+)\)", read("src/pokemon.c")):
    if m.group(1) in dex:
        to_dex[m.group(1)] = dex[m.group(1)]

# names
names = {}
for m in re.finditer(r"\[SPECIES_(\w+)\]\s*=\s*_\(\"(.*?)\"\)", read("src/data/text/species_names.h")):
    names[m.group(1)] = m.group(2)
def pretty(raw):
    raw = raw.replace("{FEMALE}", "♀").replace("{MALE}", "♂")
    s = raw.title()
    return {"Farfetch'D": "Farfetch'd"}.get(s, s)

# icons
gfx = read("src/graphics.c") + read("src/data/graphics/pokemon.h")
icon_path = {}
for m in re.finditer(r"const u8 (gMonIcon_\w+)\[\]\s*=\s*INCGFX_U8\(\"([^\"]+)\"", gfx):
    icon_path[m.group(1)] = m.group(2)
ic_src = read("src/pokemon_icon.c")
table = re.search(r"gMonIconTable\[\]\s*=\s*\{(.*?)\n\};", ic_src, re.S).group(1)
species_icon = {m.group(1): m.group(2) for m in re.finditer(r"\[SPECIES_(\w+)\]\s*=\s*(gMonIcon_\w+)", table)}
pal_src = re.search(r"gMonIconPaletteIndices\[\]\s*=\s*\{(.*?)\n\};", ic_src, re.S).group(1)
species_pal = {m.group(1): int(m.group(2)) for m in re.finditer(r"\[SPECIES_(\w+)\]\s*=\s*(\d+)", pal_src)}

def load_pal(i):
    lines = read(f"graphics/pokemon/icon_palettes/icon_palette_{i}.pal").split()
    n = int(lines[2])
    cols = [tuple(int(x) for x in lines[3 + 3 * k:6 + 3 * k]) for k in range(n)]
    return cols + [(0, 0, 0)] * (16 - n)
pals = [load_pal(i) for i in range(3)]

COLS, CELL = 16, 32
max_id = max(i for k, i in ids.items() if k == "EGG")
rows = (max_id + COLS) // COLS
atlas = Image.new("RGBA", (COLS * CELL, rows * CELL), (0, 0, 0, 0))
species = {}
missing = []
for name, sid in sorted(ids.items(), key=lambda kv: kv[1]):
    if sid > max_id or name.startswith("OLD_UNOWN") or name == "NONE":
        continue
    icon = species_icon.get(name)
    path = icon_path.get(icon) if icon else None
    if not path or not (root / path).exists():
        missing.append(name)
        continue
    im = Image.open(root / path)
    if im.mode != "P":
        missing.append(name + " (not indexed)")
        continue
    pal = pals[species_pal.get(name, 0)]
    top = im.crop((0, 0, 32, 32))
    px = top.load()
    cell = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
    cp = cell.load()
    for y in range(32):
        for x in range(32):
            i = px[x, y]
            if i != 0:
                cp[x, y] = pal[i] + (255,)
    atlas.paste(cell, ((sid % COLS) * CELL, (sid // COLS) * CELL))
    species[sid] = {"name": "Egg" if name == "EGG" else pretty(names.get(name, name)), "dex": to_dex.get(name, 0)}

atlas.save(out / "icons.png", optimize=True)
(out / "species.json").write_text(json.dumps({"cell": CELL, "cols": COLS, "species": species}, separators=(",", ":")))
print("species:", len(species), "missing:", missing)
