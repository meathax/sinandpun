#!/usr/bin/env python3
"""Match RVL_SDK/NW4R initialized-data symbols into R2VE01's data sections.

Complements sdk_match4.py (code): STT_OBJECT symbols from .data/.sdata member
sections, reloc-masked, matched positionally into the game's .data/.sdata/
.sdata2/.rodata. Zero-initialized symbols (.bss/.sbss) cannot be matched by
bytes; they are reported for code-xref recovery instead.

Output tools/sdk_data_map.txt: addr, name, lib, section, size.
"""
import struct
import sys
from pathlib import Path
from collections import defaultdict

ROOT = Path(r"D:/Arcade/AI/aCORES/Sin")
SDK_LIBS = ROOT / "build/sdk/x/wii_development_package/RVL_SDK/RVL/lib"
NW4R_LIBS = ROOT / "build/sdk/x/wii_development_package/NintendoWare/Revolution/Library/lib/RVL/Release"
DOL_PATH = ROOT / "orig/R2VE01/sys/main.dol"
OUT_PATH = ROOT / "tools/sdk_data_map.txt"

TARGET_SECTIONS = (".data", ".sdata", ".sdata2", ".rodata")


def read_dol_sections(path):
    d = path.read_bytes()
    offsets = struct.unpack_from(">18I", d, 0)
    addrs = struct.unpack_from(">18I", d, 0x48)
    lens = struct.unpack_from(">18I", d, 0x90)
    out = []
    for i in range(18):
        if offsets[i] and lens[i] and addrs[i]:
            out.append((addrs[i], lens[i], d[offsets[i]:offsets[i] + lens[i]]))
    return out


def elf_data_symbols(lib_path):
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
            e_shoff = struct.unpack_from(">I", body, 0x20)[0]
            e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", body, 0x2E)
            sh = [struct.unpack_from(">10I", body, e_shoff + i * e_shentsize) for i in range(e_shnum)]
            stroff = sh[e_shstrndx][4]
            names = []
            for s in sh:
                end = body.index(b"\x00", stroff + s[0])
                names.append(body[stroff + s[0]:end].decode("ascii", "replace"))
            symtab = next(s for n, s in zip(names, sh) if n == ".symtab")
            strtab = sh[symtab[6]]
            for si, secname in enumerate(names):
                if secname not in TARGET_SECTIONS:
                    continue
                sec = sh[si]
                if sec[5] == 0:
                    continue
                rel = next((s for n, s in zip(names, sh)
                            if n in (f".rela{secname}", f".rel{secname}")), None)
                sbase, ssize, soff = sec[3], sec[5], sec[4]
                sbytes = body[soff:soff + ssize]
                if len(sbytes) != ssize:
                    continue
                masked = set()
                if rel is not None:
                    entsz = rel[9] or 12
                    for i in range(rel[5] // entsz):
                        r_offset, r_info = struct.unpack_from(">II", body, rel[4] + i * entsz)
                        r_type = r_info & 0xFF
                        if r_type in (4, 5, 6, 109):
                            r_offset &= ~3
                        masked.add(r_offset - sbase)
                for i in range(symtab[5] // 16):
                    st_name, st_value, st_size, st_info, _o, st_shndx = \
                        struct.unpack_from(">IIIBBH", body, symtab[4] + i * 16)
                    if (st_info & 0xF) != 1 or st_size == 0 or st_shndx != si:
                        continue
                    lo = st_value - sbase
                    hi = lo + st_size
                    if lo < 0 or hi > ssize:
                        continue
                    fname = body[strtab[4] + st_name:body.index(b"\x00", strtab[4] + st_name)]
                    fname = fname.decode("ascii", "replace")
                    if not fname or fname.startswith("@"):
                        continue
                    yield fname, sbytes[lo:hi], set(o - lo for o in masked if lo <= o < hi), secname
        except Exception:
            continue


def main():
    sections = read_dol_sections(DOL_PATH)
    name_of = {}
    idx = {}
    for ea, ln, b in sections:
        secname = {0x802D5280: ".rodata", 0x802F5260: ".data", 0x804AF700: ".sdata",
                   0x804B2920: ".sdata2"}.get(ea)
        if secname is None:
            continue
        name_of[ea] = secname
        tw = struct.unpack_from(f">{ln // 4}I", b, 0)
        by_word = defaultdict(list)
        for i, w in enumerate(tw):
            if w != 0:
                by_word[w].append(i)
        idx[secname] = (ea, ln, b, by_word)

    libs = sorted(SDK_LIBS.glob("*.a")) + sorted(NW4R_LIBS.glob("*.a"))
    libs = [l for l in libs if not l.name.endswith("D.a") and "stub" not in l.name]

    results = {}
    per_lib = defaultdict(lambda: [0, 0])
    for lib in libs:
        for fname, dbytes, masked, secname in elf_data_symbols(lib):
            if secname not in idx:
                continue
            per_lib[lib.name][1] += 1
            ea, ln, b, by_word = idx[secname]
            if len(dbytes) < 4:
                continue
            n = len(dbytes) // 4
            words = [struct.unpack_from(">I", dbytes, i * 4)[0] for i in range(n)]
            key_idx = next((i for i in range(n) if i * 4 not in masked and words[i] != 0), None)
            if key_idx is None:
                continue
            hits = []
            for pos in by_word.get(words[key_idx], ()):
                p = pos - key_idx
                if p < 0 or p + n > len(b) // 4:
                    continue
                good = True
                for i in range(n):
                    if i * 4 in masked:
                        continue
                    if struct.unpack_from(">I", b, (p + i) * 4)[0] != words[i]:
                        good = False
                        break
                if good:
                    hits.append(p)
            if len(hits) == 1:
                results.setdefault(fname, (ea + hits[0] * 4, lib.name, secname, len(dbytes)))
                per_lib[lib.name][0] += 1

    print(f"data symbols matched: {sum(v[0] for v in per_lib.values())} of "
          f"{sum(v[1] for v in per_lib.values())}")
    for k, (m, t) in sorted(per_lib.items(), key=lambda kv: -kv[1][1])[:15]:
        if t:
            print(f"  {k:24s} {m:4d}/{t:4d}")

    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write("# SDK data symbol -> R2VE01 address (reloc-masked unique match)\n")
        for nm in sorted(results):
            a, lib, sec, sz = results[nm]
            f.write(f"{a:08X}\t{nm}\t{lib}\t{sec}\t{sz:#x}\n")
    print(f"wrote {OUT_PATH}: {len(results)} names")


if __name__ == "__main__":
    sys.exit(main())
