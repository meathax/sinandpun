#!/usr/bin/env python3
"""Positional byte-matcher: SDK/NW4R library functions -> R2VE01 main.dol addresses.

Indexes the game .text by word value, then verifies each SDK function at every
candidate position (masked words are wildcards). A match is REPORTED with a
quality flag: 'start' when the address is a known function start (MAP.txt),
'interior' when it sits inside a larger function-table entry.

Emits tools/sdk_name_map.txt with columns: addr, name, lib, quality.
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


def read_dol_text(path):
    d = path.read_bytes()
    offsets = struct.unpack_from(">18I", d, 0)
    addrs = struct.unpack_from(">18I", d, 0x48)
    lens = struct.unpack_from(">18I", d, 0x90)
    for i in range(18):
        if addrs[i] == 0x8001F240:
            return addrs[i], lens[i], d[offsets[i]:offsets[i] + lens[i]]
    raise RuntimeError(".text not found")


def elf_functions(lib_path):
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
            text = next(s for n, s in zip(names, sh) if n == ".text")
            if not text or text[5] == 0:
                continue
            rel = next((s for n, s in zip(names, sh) if n in (".rela.text", ".rel.text")), None)
            symtab = next(s for n, s in zip(names, sh) if n == ".symtab")
            strtab = sh[symtab[6]]
            tbase, tsize, toff = text[3], text[5], text[4]
            tbytes = body[toff:toff + tsize]
            if len(tbytes) != tsize:
                continue
            masked = set()
            if rel is not None:
                entsz = rel[9] or 12
                for i in range(rel[5] // entsz):
                    r_offset, _ = struct.unpack_from(">II", body, rel[4] + i * entsz)
                    masked.add(r_offset - tbase)
            for i in range(symtab[5] // 16):
                st_name, st_value, st_size, st_info, _o, st_shndx = \
                    struct.unpack_from(">IIIBBH", body, symtab[4] + i * 16)
                if (st_info & 0xF) != 2 or st_size == 0 or st_shndx == 0:
                    continue
                raw = struct.unpack_from(">10I", body, e_shoff + st_shndx * e_shentsize)
                if raw[4] != toff or raw[5] != tsize:
                    continue
                lo = st_value - tbase
                hi = lo + st_size
                if lo < 0 or hi > tsize:
                    continue
                fname = body[strtab[4] + st_name:body.index(b"\x00", strtab[4] + st_name)]
                fname = fname.decode("ascii", "replace")
                if not fname or fname.startswith("@") or fname.startswith("._"):
                    continue
                yield fname, tbytes[lo:hi], set(o - lo for o in masked if lo <= o < hi)
        except Exception:
            continue


def main():
    text_addr, text_len, text = read_dol_text(DOL_PATH)
    text_end = text_addr + text_len
    print(f"DOL .text: {text_addr:08X}..{text_end:08X}")
    starts = set()
    for line in MAP_PATH.read_text().splitlines():
        parts = line.split()
        if len(parts) >= 2:
            try:
                starts.add(int(parts[0], 16))
            except ValueError:
                pass

    tw = struct.unpack_from(f">{text_len // 4}I", text, 0)
    by_word = defaultdict(list)
    for i, w in enumerate(tw):
        by_word[w].append(i)

    libs = sorted(SDK_LIBS.glob("*.a")) + sorted(NW4R_LIBS.glob("*.a"))
    libs = [l for l in libs if not l.name.endswith("D.a") and "stub" not in l.name]

    results = {}
    per_lib = {}
    ambiguous = 0
    total = 0
    for lib in libs:
        ok = tot = 0
        for fname, tbytes, masked in elf_functions(lib):
            tot += 1
            total += 1
            n = len(tbytes) // 4
            words = [struct.unpack_from(">I", tbytes, i * 4)[0] for i in range(n)]
            key_idx = next((i for i in range(n) if i * 4 not in masked), None)
            if key_idx is None:
                continue
            hits = []
            for pos in by_word.get(words[key_idx], ()):
                if pos + key_idx >= len(tw):
                    continue
                p = pos - key_idx
                if p < 0:
                    continue
                good = True
                for i in range(n):
                    if i * 4 in masked:
                        continue
                    if tw[p + i] != words[i]:
                        good = False
                        break
                if good:
                    hits.append(p)
                    if len(hits) > 4:
                        break
            if not hits:
                continue
            clean = [h for h in hits if (text_addr + h * 4) in starts]
            chosen = clean if clean else hits
            if len(chosen) == 1:
                h = chosen[0]
                quality = "start" if (text_addr + h * 4) in starts else "interior"
                results.setdefault(fname, (text_addr + h * 4, lib.name, quality))
                ok += 1
            else:
                ambiguous += 1
        per_lib[lib.name] = (ok, tot)

    print(f"functions scanned: {total}, unique: {sum(v[0] for v in per_lib.values())}, "
          f"ambiguous(multi-hit): {ambiguous}")
    for k, (m, t) in sorted(per_lib.items(), key=lambda kv: -kv[1][1])[:20]:
        if t:
            print(f"  {k:24s} {m:4d}/{t:4d}")

    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write("# SDK function -> R2VE01 address (reloc-masked positional match)\n")
        for nm in sorted(results):
            a, lib, q = results[nm]
            f.write(f"{a:08X}\t{nm}\t{lib}\t{q}\n")
    print(f"wrote {OUT_PATH}: {len(results)} names")


if __name__ == "__main__":
    sys.exit(main())
