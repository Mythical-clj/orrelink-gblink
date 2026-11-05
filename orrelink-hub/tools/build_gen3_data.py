#!/usr/bin/env python3
"""Builds public/gen3/species.json from a pret/pokeemerald checkout.

  python3 tools/build_gen3_data.py /path/to/pokeemerald

species.json maps Gen 3 internal species ids (what a save stores; Hoenn species start at 277) to
{name, dex} where dex is the National Dex number. Names and numbers only: no artwork.
"""
import json, re, sys
from pathlib import Path

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

species = {}
for name, sid in sorted(ids.items(), key=lambda kv: kv[1]):
    if name.startswith("OLD_UNOWN") or name in ("NONE", "EGG") or name not in names:
        continue
    species[sid] = {"name": pretty(names[name]), "dex": to_dex.get(name, 0)}

(out / "species.json").write_text(json.dumps({"species": species}, separators=(",", ":"), ensure_ascii=False), encoding="utf-8")
print("species:", len(species))
