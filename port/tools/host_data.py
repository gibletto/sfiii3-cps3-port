#!/usr/bin/env python3
"""Copies the game's data files so they are laid out in memory as on the arcade board.

    python port/tools/host_data.py [--big-endian] [--images <name>:<size>,...] [--src <code dir>]
                                   [--data <data dir>] <out dir> [<data .c files...>]

--data takes every .c file in the folder, in the order CMake's file(GLOB) gives them.

The game finds some data by position (a script's header is the array just before it, sound sequences are
offsets into the arrays that follow). GCC pads larger arrays, so each array definition gets an explicit
alignment equal to its element's. The build compiles the copies; the game's own files are left as they are.

Some reads run past the end of a table into the next one, which on a little-endian host holds its 16- and 32-bit
elements in the other byte order. Each copy also lists its runs of such tables (host_ranges_<file>, in a section
of their own, so the game's own layout is untouched), and <out dir>/host_data_ranges.c lists the lists, for
port/cps3/host_board.c to read such bytes in the board's order.

--images names the 16- and 32-bit tables the game reads a byte at a time, with their element size: on a
little-endian host they are byte-swapped so the bytes come out in the board's order. The list is checked against
the data files (each must be there, with elements of that size) and, with --src, against the code: a 16- or
32-bit table the code reads through a byte pointer must be in it.
"""
import pathlib, re, sys

BYTE_IMAGES = {}        # --images: name -> element size
# 32-bit tables the code passes on as a byte pointer but reads as words: the sound data (sound_init)
WORD_TABLES = {"snd_seq_rom_data"}
LITTLE_ENDIAN = True    # --big-endian for big-endian targets, which take the tables as they are
NUMBER = re.compile(r"\b(0[xX][0-9A-Fa-f]+|\d+)\b")


def swap16(m):
    v = int(m.group(1), 0) & 0xFFFF
    return "0x%04X" % (((v & 0xFF) << 8) | (v >> 8))


def swap32(m):
    v = int(m.group(1), 0) & 0xFFFFFFFF
    return "0x%08X" % int.from_bytes(v.to_bytes(4, "big"), "little")


DEF = re.compile(r"^(?P<decl>(?:static\s+)?const\s+[^=;(]*?\b(?P<name>[A-Za-z_]\w*)\s*(?:\[[^\]]*\])+)\s*=")
UNIT = {"s16": 2, "u16": 2, "s32": 4, "u32": 4}     # element types held in the host's byte order, and their size
TYPES = {}      # the tables' element types, from the data files
# a cast of a table itself (not of one of its elements) to a byte pointer
BYTE_CAST = re.compile(r"\(\s*(?:const\s+)?(?:u8|s8|char|unsigned\s+char|signed\s+char)\s*(?:const\s*)?\*\s*\)"
                       r"\s*&?\s*\(?\s*([A-Za-z_]\w*)\b(?!\s*\[)")
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
            TYPES[name] = " ".join(base)
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
                line = NUMBER.sub(swap16 if BYTE_IMAGES[image] == 2 else swap32, code) + sep + comment
        out.append(line)
    close()
    if ranges:
        out += ["", RANGE_TYPE, f"struct host_range host_ranges_{stem(src)}[] __attribute__((section(\"host_ranges\"))) = {{"] + ranges + ["    { 0, 0, 0, 0 },", "};"]
    return "\n".join(out) + "\n", bool(ranges)


def check_images(code):
    """What is wrong with the --images list: a table the data files don't have or with other elements, or (with
    the code directory) a 16- or 32-bit table the code reads through a byte pointer that isn't in it."""
    problems = []
    for name, size in sorted(BYTE_IMAGES.items()):
        if name not in TYPES:
            problems.append(f"{name} is not in the data files")
        elif UNIT.get(TYPES[name]) != size:
            problems.append(f"{name} has {TYPES[name]} elements, not {size}-byte ones")
    if code:
        for f in sorted(pathlib.Path(code).rglob("*.c")):
            text = f.read_text(encoding="utf-8", errors="surrogateescape")
            for m in BYTE_CAST.finditer(text):
                name = m.group(1)
                if UNIT.get(TYPES.get(name)) and name not in BYTE_IMAGES and name not in WORD_TABLES:
                    line = text.count("\n", 0, m.start()) + 1
                    problems.append(f"{f.name}:{line} reads {name} ({TYPES[name]}) through a byte pointer, "
                                    "but it is not in --images")
    return problems


def stem(src):
    return re.sub(r"\W", "_", src.stem)


def write(dst, text):
    if not dst.exists() or dst.read_text(encoding="utf-8", errors="surrogateescape") != text:
        dst.write_text(text, encoding="utf-8", errors="surrogateescape", newline="\n")


def main():
    global LITTLE_ENDIAN
    args = sys.argv[1:]
    code = None
    data_dir = None
    while args and args[0].startswith("--"):
        opt = args.pop(0)
        if opt == "--big-endian":
            LITTLE_ENDIAN = False
        elif opt == "--images":
            for item in args.pop(0).split(","):
                name, _, size = item.partition(":")
                BYTE_IMAGES[name.strip()] = int(size)
        elif opt == "--src":
            code = args.pop(0)
        elif opt == "--data":
            # the data folder's .c files, in CMake's glob order: a list of them all is too long for a Windows
            # command line once the checkout's path is long
            data_dir = args.pop(0)
        else:
            sys.exit(f"host_data.py: unknown option {opt}")
    outdir = pathlib.Path(args[0])
    outdir.mkdir(parents=True, exist_ok=True)
    names = args[1:]
    if data_dir is not None:
        names += sorted(str(p).replace("\\", "/") for p in pathlib.Path(data_dir).glob("*.c"))
    lists = []
    for name in names:
        src = pathlib.Path(name).resolve()
        text, listed = transform(src.read_text(encoding="utf-8", errors="surrogateescape"), src)
        write(outdir / src.name, text)
        if listed:
            lists.append(f"host_ranges_{stem(src)}")
    problems = check_images(code)
    if problems:
        sys.exit("host_data.py: the byte images (--images) don't fit the game:\n  " + "\n  ".join(problems))
    write(outdir / "host_data_ranges.c",
          "/* the data files' lists of their 16- and 32-bit tables (port/tools/host_data.py) */\n" + RANGE_TYPE + "\n"
          + "".join(f"extern struct host_range {n}[];\n" for n in lists)
          + "struct host_range* const host_data_ranges[] = {\n" + "".join(f"    {n},\n" for n in lists) + "    0,\n};\n")


if __name__ == "__main__":
    main()
