#!/usr/bin/env python3
"""Dump gs2.sel exports (RSO-format static export list) and measure how much of the
donor runtime's required guest-address surface it can name for R2VE01.

Outputs sel_exports.txt (name<TAB>addr) and prints coverage:
- donor native-index registration addresses -> donor MAP names -> in sel?
- donor MKW_GADDR identity addresses  -> donor MAP names -> in sel?
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(r"D:/Arcade/AI/aCORES/Sin")
import types
_src = (ROOT / "tools/rso_link.py").read_text(encoding="utf-8")
_lines = _src.rstrip().splitlines()
if _lines[-1].strip() == "main()":
    _lines = _lines[:-1]
rso_link = types.ModuleType("rso_link_mod")
exec("\n".join(_lines), rso_link.__dict__)
assert hasattr(rso_link, "parse")

MAP_LINE = re.compile(r"^([0-9A-Fa-f]{8})\s+(\S+)\s*$")


def load_map(path):
    addr2name = {}
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = MAP_LINE.match(line.strip())
        if m:
            addr2name.setdefault(int(m.group(1), 16), m.group(2))
    return addr2name


def main():
    sel = rso_link.parse(str(ROOT / "orig/R2VE01/files/gs2.sel"))
    exports = {}
    for name, sec, off in sel["exports"]:
        # sel section ids use the DOL section mapping from rso_link
        exports[name] = (sec, off)
    print(f"gs2.sel: {len(exports)} export names")

    with open(ROOT / "tools/sel_exports.txt", "w", encoding="utf-8") as f:
        for name in sorted(exports):
            sec, off = exports[name]
            f.write(f"{name}\t{sec}\t0x{off:X}\n")

    donor = load_map(ROOT / "reference/Wiicompiled/projects/mkwii/MAP.txt")
    index = json.loads((ROOT / "runtime/native-index.json").read_text())
    regs = {}
    for r in index["registrations"]:
        regs.setdefault(int(r["address"], 16), r["symbol"])

    import re as _re
    src_files = list((ROOT / "runtime/src").rglob("*.cpp")) + list((ROOT / "runtime/src").rglob("*.h"))
    gaddr = set()
    pat = _re.compile(r"MKW_GADDR(?:_CT)?\(([0-9A-Fa-f]{8})\)")
    for p in src_files:
        for m in pat.finditer(p.read_text(encoding="utf-8", errors="replace")):
            gaddr.add(int(m.group(1), 16))

    def coverage(addrs, label):
        named = [(a, donor.get(a)) for a in sorted(addrs)]
        have_name = [(a, n) for a, n in named if n]
        in_sel = [(a, n) for a, n in have_name if n in exports]
        print(f"{label}: {len(addrs)} addrs, {len(have_name)} named in donor map, "
              f"{len(in_sel)} of those named by gs2.sel")
        return in_sel, [(a, n) for a, n in have_name if n not in exports], [a for a, n in named if not n]

    reg_in, reg_out, reg_noname = coverage(regs, "native registrations")
    ga_in, ga_out, ga_noname = coverage(gaddr, "MKW_GADDR identities")

    print("\nSample registrations missing from sel:")
    for a, n in reg_out[:25]:
        print(f"  {a:08X}  {n}")
    print("\nSample GADDR identities missing from sel:")
    for a, n in ga_out[:25]:
        print(f"  {a:08X}  {n}")
    print(f"\nregistrations with no donor-map name at all: {len(reg_noname)}")
    print(f"GADDR identities with no donor-map name: {len(ga_noname)}")


if __name__ == "__main__":
    main()
