#!/usr/bin/env python3
"""Checks the data layout the game relies on, after linking.

    python port/tools/check_layout.py <exe> [nm]

Each script header X_head must be exactly 8 bytes before its script X (the game reads it from there), and
the sound sequence offsets must land on their arrays.
"""
import pathlib, re, subprocess, sys

exe = sys.argv[1]
nm = sys.argv[2] if len(sys.argv) > 2 else "nm"
addr = {}
for line in subprocess.run([nm, exe], capture_output=True, text=True, check=True).stdout.splitlines():
    parts = line.split()
    if len(parts) == 3:
        name = parts[2].lstrip("_")
        addr[name] = int(parts[0], 16)

bad = []
pairs = 0
for name, a in addr.items():
    if name.endswith("_head"):
        table = name[:-5]
        if table in addr:
            pairs += 1
            if addr[table] - a != 8:
                bad.append((name, a, addr[table]))
# snd_seq_rom_data holds byte offsets from its start to the snd_seq_NNNN arrays after it
seq = [a for n, a in addr.items() if re.fullmatch(r"snd_seq_\d{4}", n)]
if "snd_seq_rom_data" in addr and seq:
    base = addr["snd_seq_rom_data"]
    src = (pathlib.Path(__file__).resolve().parents[2] / "games/sf3/data/snd_seq.c").read_text()
    body = src[src.index("snd_seq_rom_data[3252] = {"):]
    body = body[body.index("{") + 1:body.index("};")]
    words = [int(w, 0) for w in re.findall(r"0x[0-9A-Fa-f]+|\b\d+\b", re.sub(r"/\*.*?\*/", "", body))]
    offsets = sorted(set(w for w in words[1:] if w))
    placed = sorted(a - base for a in seq)
    missing = [o for o in offsets if o not in set(placed)]
    if missing:
        bad.append((f"snd_seq offsets not at an array ({len(missing)})", base + missing[0], 0))

print(f"check_layout: {pairs} script headers, {len(seq)} sound sequences, {len(bad)} problems")
for name, a, t in bad[:20]:
    print(f"  {name} at {a:08X}, script at {t:08X}")
sys.exit(1 if bad or pairs == 0 else 0)
