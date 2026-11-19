#!/usr/bin/env python3
"""SDK/NW4R -> R2VE01 matcher v4: exact anchors, then fuzzy + cluster disambiguation.

Pass 1: exact (0-diff) matches with >= 8 constrained words become per-library
anchors and reveal each library's address cluster in the DOL.
Pass 2a: fuzzy matches (small diff budget scaled by size) for functions whose
SDK build drifted from the game's link (KPAD/THP/GX between Feb 2009 and the
archive's Aug 2010).
Pass 2b: tiny functions (fewer than 8 constrained words) resolve only through
their library's cluster; the unique in-cluster candidate is accepted.

Output tools/sdk_name_map.txt: addr, name, lib, quality(start/interior),
diff count.
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

MIN_STRONG = 8          # constrained words for an exact anchor
FUZZY_FRAC = 1 / 24     # diff budget = max(2, words * 1/24)
CLUSTER_WINDOW = 0x8000  # same-library proximity window


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
                    r_offset, r_info = struct.unpack_from(">II", body, rel[4] + i * entsz)
                    r_type = r_info & 0xFF
                    # CW 16-bit relocs (4/5/6/109) address the halfword
                    # operand, so r_offset lands 2 past the word: align down.
                    if r_type in (4, 5, 6, 109):
                        r_offset &= ~3
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


def scan(tw, by_word, words, masked, budget):
    """All positions where the masked function matches within the diff budget."""
    n = len(words)
    key_idx = next((i for i in range(n) if i * 4 not in masked), None)
    if key_idx is None:
        return []
    hits = []
    for pos in by_word.get(words[key_idx], ()):
        p = pos - key_idx
        if p < 0 or p + n > len(tw):
            continue
        diffs = 0
        for i in range(n):
            if i * 4 in masked:
                continue
            if tw[p + i] != words[i]:
                diffs += 1
                if diffs > budget:
                    break
        if diffs <= budget:
            hits.append((diffs, p))
    hits.sort()
    return hits


def main():
    text_addr, text_len, text = read_dol_text(DOL_PATH)
    print(f"DOL .text: {text_addr:08X}..{text_addr + text_len:08X}")
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

    per_lib_funcs = {}
    for lib in libs:
        flist = list(elf_functions(lib))
        if flist:
            per_lib_funcs[lib.name] = flist

    results = {}

    # Pass 1: exact, strong only -> anchors.
    clusters = defaultdict(list)
    stats = {}
    for lib_name, flist in per_lib_funcs.items():
        ok = 0
        for fname, tbytes, masked in flist:
            words = [struct.unpack_from(">I", tbytes, i * 4)[0] for i in range(len(tbytes) // 4)]
            constrained = len([i for i in range(len(words)) if i * 4 not in masked])
            if constrained < MIN_STRONG:
                continue
            hits = scan(tw, by_word, words, masked, 0)
            if len(hits) == 1:
                a = text_addr + hits[0][1] * 4
                results.setdefault(fname, (a, lib_name, "start" if a in starts else "interior", 0))
                clusters[lib_name].append(a)
                ok += 1
        stats[lib_name] = ok
    print(f"pass1 exact anchors: {sum(stats.values())}")

    # Cluster medians.
    def cluster_center(lib):
        xs = sorted(clusters.get(lib, []))
        return xs[len(xs) // 2] if xs else None

    # Pass 2: everything else, fuzzy + cluster preference.
    pass2 = pass2_cluster = pass2_fuzzy = 0
    for lib_name, flist in per_lib_funcs.items():
        center = cluster_center(lib_name)
        for fname, tbytes, masked in flist:
            if fname in results:
                continue
            words = [struct.unpack_from(">I", tbytes, i * 4)[0] for i in range(len(tbytes) // 4)]
            n = len(words)
            constrained = len([i for i in range(n) if i * 4 not in masked])
            if constrained >= MIN_STRONG:
                budget = max(4, n // 16)
                hits = scan(tw, by_word, words, masked, budget)
                if len(hits) == 1:
                    a = text_addr + hits[0][1] * 4
                    results.setdefault(fname, (a, lib_name, "start" if a in starts else "interior", hits[0][0]))
                    pass2_fuzzy += 1
                    continue
                in_cluster = [h for h in hits if abs((text_addr + h[1] * 4) - center) <= CLUSTER_WINDOW] if center else []
                if len(in_cluster) == 1:
                    a = text_addr + in_cluster[0][1] * 4
                    results.setdefault(fname, (a, lib_name, "start" if a in starts else "interior", in_cluster[0][0]))
                    pass2_fuzzy += 1
                    continue
            else:
                hits = scan(tw, by_word, words, masked, 0)
            if center is None or not hits:
                continue
            in_cluster = [h for h in hits if abs((text_addr + h[1] * 4) - center) <= CLUSTER_WINDOW]
            if len(in_cluster) == 1:
                a = text_addr + in_cluster[0][1] * 4
                results.setdefault(fname, (a, lib_name, "start" if a in starts else "interior", in_cluster[0][0]))
                if constrained < MIN_STRONG:
                    pass2_cluster += 1
                else:
                    pass2_fuzzy += 1
    print(f"pass2 fuzzy: {pass2_fuzzy}, cluster-resolved: {pass2_cluster}")
    print(f"total: {len(results)}")

    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write("# SDK function -> R2VE01 address (anchor+fuzzy+cluster match)\n")
        for nm in sorted(results):
            a, lib, q, d = results[nm]
            f.write(f"{a:08X}\t{nm}\t{lib}\t{q}\tdiff={d}\n")
    print(f"wrote {OUT_PATH}: {len(results)} names")


if __name__ == "__main__":
    sys.exit(main())
