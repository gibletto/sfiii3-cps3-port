#!/usr/bin/env python3
"""Converts a 3rd Strike ROM set into the files the port loads.

    python prepare_rom.py <sfiii3nr1.zip> <out dir>

Writes bios.bin (decrypted BIOS), cg.bin (decrypted graphics patterns) and user5.bin (graphics and sound ROMs).
"""
import struct, sys, zipfile
from pathlib import Path

# cps3rom.py (decryption, SIMM reading) is next to this file in a release, or in the decomp's tools
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "games/sf3/tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import cps3rom  # noqa: E402


def bios(z):
    names = z.namelist()    # the set has two BIOS chips; the port uses the European one
    name = "sfiii3_euro.29f400.u2"
    if name not in names:
        name = next(n for n in names if n.endswith(".29f400.u2"))
    data = z.read(name)
    out = bytearray(len(data))
    for i in range(0, len(data), 4):
        w = int.from_bytes(data[i:i + 4], "big") ^ cps3rom.mask(i)
        out[i:i + 4] = w.to_bytes(4, "big")
    return out


def user5(z):
    out = bytearray()
    for simm in (3, 4, 5, 6):
        for pair in range(0, 8, 2):
            f0 = z.read(f"sfiii3-simm{simm}.{pair}")
            f1 = z.read(f"sfiii3-simm{simm}.{pair + 1}")
            words = [(f0[i] << 8) | (f0[i + 1] << 24) | f1[i] | (f1[i + 1] << 16) for i in range(0, len(f0), 2)]
            out += struct.pack(f"<{len(words)}I", *words)
    return out


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    zpath, outdir = Path(sys.argv[1]), Path(sys.argv[2])
    outdir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zpath) as z:
        (outdir / "bios.bin").write_bytes(bios(z))
        flat = cps3rom.read_simm(z, 2, 0x06800000)
        (outdir / "cg.bin").write_bytes(flat[:cps3rom.CG_END - cps3rom.CG_START])
        (outdir / "user5.bin").write_bytes(user5(z))
    print(f"wrote {outdir}")


if __name__ == "__main__":
    main()
