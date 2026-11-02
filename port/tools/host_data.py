#!/usr/bin/env python3
"""Copies the game's data files so they are laid out in memory as on the arcade board.

    python port/tools/host_data.py [--big-endian] <out dir> <data .c files...>

The game finds some data by position (a script's header is the array just before it, sound sequences are
offsets into the arrays that follow). GCC pads larger arrays, so each array definition gets an explicit
alignment equal to its element's. The build compiles the copies; games/sf3 is left as it is.

Some reads run past the end of a table into the next one, which on a little-endian host holds its 16- and 32-bit
elements in the other byte order. Each copy also lists its runs of such tables (host_ranges_<file>, in a section
of their own, so the game's own layout is untouched), and <out dir>/host_data_ranges.c lists the lists, for
port/cps3/host_board.c to read such bytes in the board's order.
"""
import pathlib, re, sys

# u16 tables the game reads a byte at a time: byte-swapped on little-endian hosts so the bytes come out in the
# board's order
BYTE_IMAGES = {"sys_font_cg", "sc_chr_data", "gill_sa_number_chr"}
LITTLE_ENDIAN = True    # --big-endian for big-endian targets, which take the tables as they are
NUMBER = re.compile(r"\b(0[xX][0-9A-Fa-f]+|\d+)\b")


def swap16(m):
    v = int(m.group(1), 0) & 0xFFFF
    return "0x%04X" % (((v & 0xFF) << 8) | (v >> 8))


DEF = re.compile(r"^(?P<decl>(?:static\s+)?const\s+[^=;(]*?\b(?P<name>[A-Za-z_]\w*)\s*(?:\[[^\]]*\])+)\s*=")
UNIT = {"s16": 2, "u16": 2, "s32": 4, "u32": 4}     # element types held in the host's byte order, and their size
# The constants (the board's ROM data) go in a section of their own, in link order, so the constants of the code
# files linked between them (jump tables, strings) don't come between them. CONST matches the ones DEF doesn't:
# single values and tables of function pointers.
SECTION = ' __attribute__((section("game_rom")))'
CONST = re.compile(r"^(?P<decl>(?:static\s+)?(?:const\s+[A-Za-z_][\w\s]*?\b[A-Za-z_]\w*"
                   r"|[A-Za-z_][\w\s]*\(\s*\*\s*const\s+[A-Za-z_]\w*\s*(?:\[[^\]]*\])+\s*\)\s*\([^)]*\)))\s*=")


def rom(m):
    """The section for a table whose elements are constant (an array of pointers to constants is not)."""
    typ = m.group("decl")[:m.start("name") - m.start("decl")]
    return SECTION if "*" not in typ or "const" in typ.rpartition("*")[2] else ""


# a run of consecutive tables of one element size: aligned to it and a whole number of elements long, they are
# contiguous, from first to the end of last
RANGE_TYPE = "struct host_range { const void* first; const void* last; unsigned last_size, unit; };"


def transform(text, src):
    out = [f'#line 1 "{src.as_posix()}"']
    image = None
    ranges, run = [], None      # run: [first, last, unit]

    def close():
        nonlocal run
        if run:
            ranges.append(f"    {{ {run[0]}, {run[1]}, sizeof({run[1]}), {run[2]} }},")
        run = None

    for line in text.splitlines():
        m = DEF.match(line)
        c = None if m else CONST.match(line)
        if c:
            line = c.group("decl") + SECTION + line[c.end("decl"):]
        elif m and line.startswith("static"):
            line = m.group("decl") + rom(m) + line[m.end("decl"):]
        elif m:
            decl, name = m.group("decl"), m.group("name")
            base = [w for w in line[:m.start("name")].split() if w != "const"]
            rest = line[m.end("decl"):]
            line = f"extern {decl}; {decl} __attribute__((aligned(__alignof__({name}[0]))))" + rom(m) + rest
            if name in BYTE_IMAGES:
                image = name
                close()
            elif len(base) == 1 and base[0] in UNIT:
                if run and run[2] == UNIT[base[0]]:
                    run[1] = name
                else:
                    close()
                    run = [name, name, UNIT[base[0]]]
            else:
                close()
        elif line.startswith(("const", "static")):     # anything else defined between two tables ends a run
            close()
        elif image:
            if line.startswith("};"):
                image = None
            elif LITTLE_ENDIAN:
                code, sep, comment = line.partition("/*")
                line = NUMBER.sub(swap16, code) + sep + comment
        out.append(line)
    close()
    if ranges:
        out += ["", RANGE_TYPE, f"struct host_range host_ranges_{stem(src)}[] __attribute__((section(\"host_ranges\"))) = {{"] + ranges + ["    { 0, 0, 0, 0 },", "};"]
    return "\n".join(out) + "\n", bool(ranges)


def stem(src):
    return re.sub(r"\W", "_", src.stem)


def write(dst, text):
    if not dst.exists() or dst.read_text(encoding="utf-8", errors="surrogateescape") != text:
        dst.write_text(text, encoding="utf-8", errors="surrogateescape", newline="\n")


def main():
    global LITTLE_ENDIAN
    args = sys.argv[1:]
    if args and args[0] == "--big-endian":
        LITTLE_ENDIAN = False
        args = args[1:]
    outdir = pathlib.Path(args[0])
    outdir.mkdir(parents=True, exist_ok=True)
    lists = []
    for name in args[1:]:
        src = pathlib.Path(name).resolve()
        text, listed = transform(src.read_text(encoding="utf-8", errors="surrogateescape"), src)
        write(outdir / src.name, text)
        if listed:
            lists.append(f"host_ranges_{stem(src)}")
    write(outdir / "host_data_ranges.c",
          "/* the data files' lists of their 16- and 32-bit tables (port/tools/host_data.py) */\n" + RANGE_TYPE + "\n"
          + "".join(f"extern struct host_range {n}[];\n" for n in lists)
          + "struct host_range* const host_data_ranges[] = {\n" + "".join(f"    {n},\n" for n in lists) + "    0,\n};\n")


if __name__ == "__main__":
    main()
