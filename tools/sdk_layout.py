#!/usr/bin/env python3
"""Resolve straggler BSS globals by sbss/bss link-layout adjacency.

For the libraries the Sin runtime depends on (vi.a, os.a, dvd.a, ax.a, gx.a,
sc.a, kpad.a, wpad.a), take every BSS symbol (name, section, size, member order)
and anchor the run against addresses already recovered by reloc transfer
(tools/sdk_globals_map.txt). Within one member's BSS contribution, symbols
link contiguously: an anchor pins the run's absolute base, and each unresolved
neighbor lands at anchor_base + sum(sizes of symbols before it). Only accepts
a symbol when the anchors on both sides agree with the predicted layout.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(r"D:/Arcade/AI/aCORES/Sin")
SDK = ROOT / "build/sdk/x/wii_development_package/RVL_SDK/RVL/lib"
OUT = ROOT / "tools/sdk_layout_map.txt"

WANT_LIBS = ("vi.a", "os.a", "dvd.a", "ax.a", "gx.a", "sc.a", "kpad.a", "wpad.a")


def members(lib):
    d = lib.read_bytes()
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
            out = {}
            for i in range(symtab[5] // 16):
                st_name, st_value, st_size, st_info, _o, st_shndx = \
                    struct.unpack_from(">IIIBBH", body, symtab[4] + i * 16)
                sec = names[st_shndx] if st_shndx < len(names) else ""
                if sec not in (".bss", ".sbss"):
                    continue
                nm = body[strtab[4] + st_name:body.index(b"\x00", strtab[4] + st_name)]
                nm = nm.decode("ascii", "replace")
                if not nm or nm.startswith("@"):
                    continue
                out.setdefault(sec, []).append((nm, st_value, st_size))
            yield out
        except Exception:
            continue


def main():
    resolved = {}
    for line in (ROOT / "tools/sdk_globals_map.txt").read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        addr, name = line.split("\t")[:2]
        resolved[name] = int(addr, 16)

    results = {}
    for libname in WANT_LIBS:
        for member_bss in members(SDK / libname):
            for sec, syms in member_bss.items():
                # sort by st_value (the link position within the member)
                syms.sort(key=lambda s: s[1])
                anchor_idx = [i for i, (n, v, sz) in enumerate(syms) if n in resolved]
                if len(anchor_idx) < 1:
                    continue
                for i, (n, v, sz) in enumerate(syms):
                    if n in resolved or i in anchor_idx:
                        continue
                    # nearest anchors before and after
                    prev = max((a for a in anchor_idx if a < i), default=None)
                    nxt = min((a for a in anchor_idx if a > i), default=None)
                    pred_before = sum(s[2] for s in syms[prev + 1:i]) if prev is not None else None
                    pred_after = sum(s[2] for s in syms[i + 1:nxt]) if nxt is not None else None
                    if prev is not None and pred_before is not None:
                        base = resolved[syms[prev][0]]
                        cand = base + sum(s[2] for s in syms[prev + 1:i + 1])
                        ok = True
                        if nxt is not None and pred_after is not None:
                            end = resolved[syms[nxt][0]]
                            ok = (cand + pred_after + sz <= end + 0 and
                                  resolved[syms[nxt][0]] - (cand + sz) == pred_after)
                        if ok:
                            results.setdefault(n, (cand, libname, sec))

    print(f"layout-resolved: {len(results)}")
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("# SDK BSS globals -> R2VE01 (sbss/bss link-layout adjacency)\n")
        for nm in sorted(results):
            a, lib, sec = results[nm]
            f.write(f"{a:08X}\t{nm}\t{lib}\t{sec}\n")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    sys.exit(main())
