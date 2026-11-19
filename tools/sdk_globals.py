#!/usr/bin/env python3
"""Harvest SDK global-variable addresses in R2VE01 via reloc-name transfer.

For every function matched by sdk_match4 (tools/sdk_name_map.txt), walk the
member's .rela.text: each entry names a symbol. Decode the same instruction
offset in the game's DOL (already linked, so the patched word carries the real
address) and extract the effective guest address:

  lis + addi/ori/stw..     -> (hi << 16) + (int16)lo
  single R_PPC_ADDR32 word -> the word itself
  sda-relative loads/stores -> SDA base + (int16)offset

Emits tools/sdk_globals_map.txt: "addr  name  via_function  kind". Also
reports conflicting resolutions for manual review.
"""
import struct
import sys
from pathlib import Path
from collections import defaultdict

ROOT = Path(r"D:/Arcade/AI/aCORES/Sin")
SDK_LIBS = ROOT / "build/sdk/x/wii_development_package/RVL_SDK/RVL/lib"
NW4R_LIBS = ROOT / "build/sdk/x/wii_development_package/NintendoWare/Revolution/Library/lib/RVL/Release"
DOL_PATH = ROOT / "orig/R2VE01/sys/main.dol"
MAP_PATH = ROOT / "tools/sdk_name_map.txt"
OUT_PATH = ROOT / "tools/sdk_globals_map.txt"

SDA13 = 0x804B7700
SDA2 = 0x804BA920


def read_dol_text(path):
    d = path.read_bytes()
    offsets = struct.unpack_from(">18I", d, 0)
    addrs = struct.unpack_from(">18I", d, 0x48)
    lens = struct.unpack_from(">18I", d, 0x90)
    for i in range(18):
        if addrs[i] == 0x8001F240:
            return addrs[i], lens[i], d[offsets[i]:offsets[i] + lens[i]]
    raise RuntimeError


def elf_reloc_symbols(lib_path):
    """Yield (function_name, [(rel_off, sym_name), ...]) for text symbols."""
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
            text = next((s for n, s in zip(names, sh) if n == ".text"), None)
            rel = next((s for n, s in zip(names, sh) if n in (".rela.text", ".rel.text")), None)
            symtab = next(s for n, s in zip(names, sh) if n == ".symtab")
            strtab = sh[symtab[6]]
            if text is None or rel is None:
                continue
            tbase, tsize = text[3], text[5]
            syms = []
            for i in range(symtab[5] // 16):
                st_name, st_value, st_size, st_info, _o, st_shndx = \
                    struct.unpack_from(">IIIBBH", body, symtab[4] + i * 16)
                nm = body[strtab[4] + st_name:body.index(b"\x00", strtab[4] + st_name)]
                syms.append((nm.decode("ascii", "replace"), st_info, st_shndx))
            entsz = rel[9] or 12
            relocs_by_func = defaultdict(list)
            # Address-forming reloc types only: 1 ADDR32, 4 ADDR16_LO,
            # 5 ADDR16_HI, 6 ADDR16_HA, 109 EMB_SDA21. Type 10 (REL24) is a
            # branch to code, not a global reference.
            KEEP_TYPES = {1, 4, 5, 6, 109}
            for i in range(rel[5] // entsz):
                r_offset, r_info = struct.unpack_from(">II", body, rel[4] + i * entsz)
                r_type = r_info & 0xFF
                if r_type not in KEEP_TYPES:
                    continue
                # CW 16-bit relocs address the halfword operand: align down.
                if r_type in (4, 5, 6, 109):
                    r_offset &= ~3
                symndx = r_info >> 8
                if symndx >= len(syms):
                    continue
                symname, sinfo, s_shndx = syms[symndx]
                if not symname or symname.startswith("@"):
                    continue
                relocs_by_func[("TEXT", r_offset - tbase, r_type)].append(symname)
            # Map reloc offsets to the containing function by st_value ranges.
            for i in range(symtab[5] // 16):
                st_name, st_value, st_size, st_info, _o, st_shndx = \
                    struct.unpack_from(">IIIBBH", body, symtab[4] + i * 16)
                if (st_info & 0xF) != 2 or st_size == 0 or st_shndx == 0:
                    continue
                raw = struct.unpack_from(">10I", body, e_shoff + st_shndx * e_shentsize)
                if raw[4] != text[4] or raw[5] != tsize:
                    continue
                fname = syms[i][0] if False else body[strtab[4] + st_name:body.index(b"\x00", strtab[4] + st_name)].decode("ascii", "replace")
                if not fname:
                    continue
                lo = st_value - tbase
                hi = lo + st_size
                for (tag, roff, rtype), names_list in relocs_by_func.items():
                    if lo <= roff < hi:
                        for nm in names_list:
                            yield fname, roff - lo, nm, rtype
        except Exception:
            continue


def decode_effective(tw, p, rtype):
    """Effective guest address referenced by the instruction at word index p."""
    # ADDR16_LO anchors on the low half; the lis (HA) is the preceding word.
    if rtype == 4:
        p = p - 1
        if p < 0:
            return None, False
    w = tw[p]
    op = w >> 26
    if rtype == 1:  # R_PPC_ADDR32: the word is the address.
        return w & 0xFFFFFFFF, False
    if op == 15:  # lis
        hi = (w & 0xFFFF) << 16
        nxt = tw[p + 1] if p + 1 < len(tw) else 0
        if (nxt >> 26) in (14, 24, 32, 36, 38):  # addi/ori/lwz/stw/stwu
            lo = nxt & 0xFFFF
            if lo >= 0x8000:
                lo -= 0x10000
            return (hi + lo) & 0xFFFFFFFF, True
        return hi & 0xFFFFFFFF, False
    if op in (32, 36, 34, 38, 40, 44, 47) or (op == 14 and rtype == 109):
        # lwz/stw/lbz/stb/lhz/sth/lfs, and addi-as-address-of for SDA symbols.
        ra = (w >> 16) & 0x1F
        off = w & 0xFFFF
        if off >= 0x8000:
            off -= 0x10000
        if ra == 13:
            return (SDA13 + off) & 0xFFFFFFFF, False
        if ra == 2:
            return (SDA2 + off) & 0xFFFFFFFF, False
        return None, False
    return None, False


def main():
    text_addr, text_len, text = read_dol_text(DOL_PATH)
    tw = struct.unpack_from(f">{text_len // 4}I", text, 0)
    base_index = {a: (a - text_addr) // 4 for a in ()}

    matched = {}
    for line in MAP_PATH.read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        addr, name, lib, q, _d = line.split("\t")
        matched.setdefault(name, (int(addr, 16), lib))

    libs = sorted(SDK_LIBS.glob("*.a")) + sorted(NW4R_LIBS.glob("*.a"))
    libs = [l for l in libs if not l.name.endswith("D.a") and "stub" not in l.name]

    globals_map = {}
    conflicts = defaultdict(set)
    harvested = 0
    for lib in libs:
        for fname, roff, symname, rtype in elf_reloc_symbols(lib):
            hit = matched.get(fname)
            if hit is None:
                continue
            sin_addr, _lib = hit
            p = (sin_addr - text_addr) // 4 + roff // 4
            if p < 0 or p >= len(tw):
                continue
            addr, paired = decode_effective(tw, p, rtype)
            if addr is None or addr == 0:
                continue
            if not (0x80000000 <= addr < 0x81800000):
                continue
            harvested += 1
            if symname in globals_map and globals_map[symname] != addr:
                conflicts[symname].add(globals_map[symname])
                conflicts[symname].add(addr)
            else:
                globals_map[symname] = addr

    print(f"relocs harvested: {harvested}, distinct globals: {len(globals_map)}, "
          f"conflicting: {len(conflicts)}")
    for k in list(conflicts)[:10]:
        print(f"  conflict {k}: " + ", ".join(f"{a:08X}" for a in sorted(conflicts[k])))

    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write("# SDK global -> R2VE01 address (reloc-name transfer through matched code)\n")
        for nm in sorted(globals_map):
            if nm in conflicts:
                continue
            f.write(f"{globals_map[nm]:08X}\t{nm}\n")
    print(f"wrote {OUT_PATH}: {len(globals_map) - len(conflicts)} names")


if __name__ == "__main__":
    sys.exit(main())
