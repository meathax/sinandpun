#!/usr/bin/env python3
"""Byte-match RVL_SDK / NW4R library functions into Sin & Punishment 2 (R2VE01) main.dol.

The SDK archives' members are CodeWarrior ELF32 objects. For every STT_FUNC
symbol the .text slice is extracted, words the linker relocates are turned into
wildcards, and each slice is matched against the game's dtk function table
(exact size + all non-relocated words equal, branch offsets included).

Emits tools/sdk_name_map.txt: "sin_addr<TAB>name<TAB>lib" for every unambiguous
match, plus per-library coverage.
"""
import struct
import sys
from pathlib import Path
from collections import defaultdict

ROOT = Path(r"D:/Arcade/AI/aCORES/Sin")
SDK_LIBS = ROOT / "build/sdk/x/wii_development_package/RVL_SDK/RVL/lib"
NW4R_LIBS = ROOT / "build/sdk/x/wii_development_package/NintendoWare/Revolution/Library/lib/RVL/Release"
DOL_PATH = ROOT / "orig/R2VE01/sys/main.dol"
MAP_PATH = ROOT / "projects/r2ve01/MAP.txt"
OUT_PATH = ROOT / "tools/sdk_name_map.txt"


def read_dol(path):
    d = path.read_bytes()
    offsets = struct.unpack_from(">18I", d, 0)
    addrs = struct.unpack_from(">18I", d, 0x48)
    lens = struct.unpack_from(">18I", d, 0x90)
    sections = []
    for i in range(18):
        if offsets[i] == 0 or lens[i] == 0:
            continue
        sections.append((addrs[i], lens[i], d[offsets[i]:offsets[i] + lens[i]]))
    return sections


def load_functions(text_addr, text, text_end):
    starts = []
    for line in MAP_PATH.read_text().splitlines():
        parts = line.split()
        if len(parts) >= 2:
            try:
                starts.append(int(parts[0], 16))
            except ValueError:
                continue
    starts = sorted(set(a for a in starts if text_addr <= a < text_end))
    funcs = []
    for i, a in enumerate(starts):
        nxt = starts[i + 1] if i + 1 < len(starts) else text_end
        size = nxt - a
        if size <= 0:
            continue
        funcs.append((a, text[a - text_addr:a - text_addr + size]))
    return funcs


def elf_sections(body):
    e_shoff = struct.unpack_from(">I", body, 0x20)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", body, 0x2E)
    sh = [struct.unpack_from(">10I", body, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    stroff = sh[e_shstrndx][4]
    named = []
    for s in sh:
        end = body.index(b"\x00", stroff + s[0])
        named.append((body[stroff + s[0]:end].decode("ascii", "replace"), s))
    return named


def extract_functions(lib_path):
    """Yield (name, text_slice, masked_word_offsets) for every STT_FUNC."""
    d = lib_path.read_bytes()
    if not d.startswith(b"!<arch>\n"):
        return
    pos = 8
    while pos + 60 <= len(d):
        hdr = d[pos:pos + 60]
        mname = hdr[0:16].decode("ascii", "replace").rstrip()
        try:
            size = int(hdr[48:58].decode("ascii", "replace").strip())
        except ValueError:
            break
        body = d[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if mname.startswith("/") or not body.startswith(b"\x7fELF"):
            continue
        try:
            named = elf_sections(body)
        except Exception:
            continue
        text = next((s for n, s in named if n == ".text"), None)
        if text is None or text[5] == 0:
            continue
        rel = next((s for n, s in named if n in (".rela.text", ".rel.text")), None)
        symtab = next((s for n, s in named if n == ".symtab"), None)
        if symtab is None:
            continue
        strtab = named[symtab[6]][1]
        tbase, tsize, toff = text[3], text[5], text[4]  # sh_addr, sh_size, sh_offset
        tbytes = body[toff:toff + tsize]
        if len(tbytes) != tsize:
            continue
        masked = set()
        if rel is not None:
            entsz = rel[9] or 12
            n = rel[5] // entsz
            for i in range(n):
                r_offset, r_info = struct.unpack_from(">II", body, rel[4] + i * entsz)
                masked.add(r_offset - tbase)
        nsyms = symtab[5] // 16
        for i in range(nsyms):
            st_name, st_value, st_size, st_info, st_other, st_shndx = \
                struct.unpack_from(">IIIBBH", body, symtab[4] + i * 16)
            if (st_info & 0xF) != 2 or st_size == 0 or st_shndx == 0:
                continue
            sec = named[st_shndx - 1][1] if 0 < st_shndx <= len(named) else None
            # st_shndx indexes the section table; find .text by identity
            if st_shndx >= len(named) + 1:
                continue
            sh_row = None
            e_shoff = struct.unpack_from(">I", body, 0x20)[0]
            e_shentsize = struct.unpack_from(">H", body, 0x2E)[0]
            raw = struct.unpack_from(">10I", body, e_shoff + (st_shndx) * e_shentsize) if st_shndx > 0 else None
            if raw is None or raw[5] == 0:
                continue
            # skip non-text sections via section name
            nm_end = body.index(b"\x00", named[st_shndx - 1] and 0 or 0) if False else None
            # simpler: compare sh_offset to text's
            if raw[4] != toff or raw[5] != tsize:
                continue
            snm_end = body.index(b"\x00", strtab[4] + st_name)
            fname = body[strtab[4] + st_name:snm_end].decode("ascii", "replace")
            if not fname or fname.startswith("@") or fname.startswith("._"):
                continue
            lo = st_value - tbase
            hi = lo + st_size
            if lo < 0 or hi > tsize:
                continue
            yield fname, tbytes[lo:hi], set(o - lo for o in masked if lo <= o < hi)


def words_masked(buf, masked):
    n = len(buf) // 4
    out = []
    for i in range(n):
        if i * 4 in masked:
            out.append(None)
        else:
            out.append(struct.unpack_from(">I", buf, i * 4)[0])
    return out


def main():
    sections = read_dol(DOL_PATH)
    text_addr, text_len, text = next((ea, ln, b) for ea, ln, b in sections if ea == 0x8001F240)
    text_end = text_addr + text_len
    print(f"DOL .text: {text_addr:08X}..{text_end:08X} ({text_len:#x} bytes)")
    funcs = load_functions(text_addr, text, text_end)
    print(f"function table: {len(funcs)} entries")
    by_size = defaultdict(list)
    for a, b in funcs:
        by_size[len(b)].append((a, b))

    libs = sorted(SDK_LIBS.glob("*.a")) + sorted(NW4R_LIBS.glob("*.a"))
    libs = [l for l in libs if not l.name.endswith("D.a") and "stub" not in l.name]
    matches = {}
    per_lib = {}
    ambiguous = 0
    total = 0
    # CodeWarrior pads function bodies to 8/16-byte alignment in st_size while
    # the game's function table trims padding: compare the slice against the
    # function head, allowing the DOL entry to be up to 16 bytes longer.
    for lib in libs:
        ok = tot = 0
        for fname, tbytes, masked in extract_functions(lib):
            tot += 1
            total += 1
            cands = [pair for sz, lst in by_size.items()
                     if sz >= len(tbytes) and sz <= len(tbytes) + 16
                     for pair in lst]
            if not cands:
                continue
            mw = words_masked(tbytes, masked)
            hits = []
            for a, b in cands:
                good = True
                for i, w in enumerate(mw):
                    if w is None:
                        continue
                    if struct.unpack_from(">I", b, i * 4)[0] != w:
                        good = False
                        break
                if good:
                    hits.append(a)
                    if len(hits) > 1:
                        break
            if len(hits) == 1:
                matches.setdefault(fname, (hits[0], lib.name))
                ok += 1
            elif len(hits) > 1:
                ambiguous += 1
        per_lib[lib.name] = (ok, tot)

    print(f"functions scanned: {total}, unique matches: {sum(v[0] for v in per_lib.values())}, "
          f"ambiguous: {ambiguous}")
    print("\nPer-library (matched/functions):")
    for k, (m, t) in sorted(per_lib.items(), key=lambda kv: -kv[1][1]):
        if t:
            print(f"  {k:24s} {m:4d}/{t:4d}")

    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write("# SDK function -> R2VE01 address (reloc-masked unique byte match)\n")
        for nm in sorted(matches):
            a, lib = matches[nm]
            f.write(f"{a:08X}\t{nm}\t{lib}\n")
    print(f"\nwrote {OUT_PATH}: {len(matches)} names")


if __name__ == "__main__":
    sys.exit(main())
