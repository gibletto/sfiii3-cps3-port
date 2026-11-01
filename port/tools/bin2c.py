#!/usr/bin/env python3
"""Turns a binary file into a C header: <name>[] (4-byte aligned) and <name>_size.

    python port/tools/bin2c.py <in> <out.h> <name>
"""
import sys


def main():
    src, out, name = sys.argv[1:4]
    data = open(src, "rb").read()
    lines = [f"/* made from {src.replace(chr(92), '/').split('/')[-1]} by port/tools/bin2c.py */",
             "#pragma once",
             f"static const unsigned char {name}[] __attribute__((aligned(4))) = {{"]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    lines.append("};")
    lines.append(f"static const unsigned int {name}_size = {len(data)};")
    open(out, "w", newline="\n").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
