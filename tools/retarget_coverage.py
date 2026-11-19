#!/usr/bin/env python3
"""Cross-map coverage: donor MKWii PAL registration addresses -> names -> Sin R2VE01 addresses.

Reads runtime/native-index.json (788 registrations), donor projects/mkwii/MAP.txt
and Sin projects/r2ve01/MAP.txt, and reports how many registrations resolve by
name, so the Sin retarget scope is measurable before any source edits.
"""
import json
import re
import sys
from collections import Counter

ROOT = r"D:/Arcade/AI/aCORES/Sin"
MAP_LINE = re.compile(r"^([0-9A-Fa-f]{8})\s+(\S+)\s*$")


def load_map(path):
    addr2name, name2addr = {}, {}
    dup = Counter()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = MAP_LINE.match(line.strip())
            if not m:
                continue
            addr, name = int(m.group(1), 16), m.group(2)
            addr2name.setdefault(addr, name)
            if name in name2addr and name2addr[name] != addr:
                dup[name] += 1
            name2addr.setdefault(name, addr)
    return addr2name, name2addr, dup


def main():
    donor = load_map(f"{ROOT}/reference/Wiicompiled/projects/mkwii/MAP.txt")
    sin = load_map(f"{ROOT}/projects/r2ve01/MAP.txt")
    print(f"donor map: {len(donor[0])} addrs, {len(donor[1])} names, {len(donor[2])} name collisions")
    print(f"sin map:   {len(sin[0])} addrs, {len(sin[1])} names, {len(sin[2])} name collisions")

    index = json.load(open(f"{ROOT}/runtime/native-index.json", encoding="utf-8"))
    regs = index["registrations"]
    print(f"registrations: {len(regs)}")

    direct, byname, unresolved = [], [], []
    seen_addr = set()
    for reg in regs:
        addr = int(reg["address"], 16)
        if addr in seen_addr:
            continue
        seen_addr.add(addr)
        dname = donor[0].get(addr)
        if dname is None:
            unresolved.append((reg["address"], reg["symbol"], "no donor name"))
            continue
        saddr = sin[1].get(dname)
        if saddr is None:
            unresolved.append((reg["address"], dname, "name not in sin map"))
            continue
        if saddr == addr:
            direct.append((addr, dname))
        else:
            byname.append((addr, dname, saddr))
    print(f"unique registration addrs: {len(seen_addr)}")
    print(f"  same address in both games: {len(direct)}")
    print(f"  resolved to a different Sin address: {len(byname)}")
    print(f"  unresolved: {len(unresolved)}")
    print("\nTop unresolved (first 40):")
    for a, n, why in unresolved[:40]:
        print(f"  {a}  {n}  [{why}]")
    names = Counter(s for _, s, _ in unresolved)
    print("\nUnresolved by source file:")
    src = Counter(reg["sourceFile"] for reg in regs
                  if (int(reg["address"], 16) not in donor[0]))
    for s, c in src.most_common(20):
        print(f"  {c:4d}  {s}")


if __name__ == "__main__":
    sys.exit(main())
