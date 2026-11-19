#!/usr/bin/env python3
"""Walk static call edges upward from guest functions to the first ancestors the game executed.

Usage: python tools/xref_up.py 0x80034980 [0x8020F1D4 ...] [--calltrace build/native/calltrace.txt]

Static `bl` edges only (indirect/vtable calls are invisible). An ancestor is "executed" when its
address appears in the first-seen call trace (SIN_CALLTRACE=1). The reported call site is where
the gate between the executed code and the wanted function lives.
"""
import argparse
import bisect
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load_starts():
    out = []
    for line in (ROOT / "projects/r2ve01/MAP.txt").read_text().splitlines():
        if line.strip() and re.match(r"^[0-9a-fA-F]{8}\s", line):
            out.append(int(line.split()[0], 16))
    return sorted(set(out))


def load_edges():
    d = (ROOT / "orig/R2VE01/sys/main.dol").read_bytes()
    offs = struct.unpack_from(">18I", d, 0)
    addrs = struct.unpack_from(">18I", d, 0x48)
    lens = struct.unpack_from(">18I", d, 0x90)
    edges = {}
    for i in range(18):
        if offs[i] == 0 or lens[i] == 0 or not (0x80003000 <= addrs[i] < 0x80400000):
            continue
        t = d[offs[i]:offs[i] + lens[i]]
        base = addrs[i]
        for o in range(0, len(t) - 4, 4):
            w = struct.unpack_from(">I", t, o)[0]
            if (w >> 26) == 18 and (w & 2) == 0:
                li = (w >> 2) & 0xFFFFFF
                if li & 0x800000:
                    li -= 0x1000000
                edges.setdefault((base + o + li * 4) & 0xFFFFFFFF, set()).add(base + o)
    return edges


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("targets", nargs="+")
    ap.add_argument("--calltrace", default=str(ROOT / "build/native/calltrace.txt"))
    ap.add_argument("--depth", type=int, default=12)
    ap.add_argument("--all", action="store_true", help="print the full static caller tree (executed or not)")
    a = ap.parse_args()
    starts = load_starts()
    edges = load_edges()
    seen = {int(l.split()[1], 16) for l in open(a.calltrace) if l.strip()}

    def fn(addr):
        return starts[bisect.bisect_right(starts, addr) - 1]

    if a.all:
        def tree(f, depth, vis):
            if f in vis or depth > a.depth:
                return
            vis.add(f)
            callers = sorted({fn(s) for s in edges.get(f, ())})
            print("  " * depth + f"fn_{f:08X} {'RAN' if f in seen else '   '} callers: "
                  + (" ".join(f"{c:08X}" for c in callers) or "(none static)"))
            for c in callers:
                tree(c, depth + 1, vis)
        for t in a.targets:
            tree(fn(int(t, 16)), 0, set())
        return

    for t in a.targets:
        start = fn(int(t, 16))
        print(f"== target fn_{start:08X}" + ("  (EXECUTED)" if start in seen else ""))
        frontier = [(start, [start])]
        visited = {start}
        shown = 0
        for _ in range(a.depth):
            nxt = []
            for f, path in frontier:
                for site in sorted(edges.get(f, ())):
                    c = fn(site)
                    if c in visited:
                        continue
                    visited.add(c)
                    if c in seen:
                        print(f"  executed ancestor fn_{c:08X}, call site {site:08X}: "
                              + " <- ".join(f"{p:08X}" for p in path + [c]))
                        shown += 1
                    else:
                        nxt.append((c, path + [c]))
            frontier = nxt
            if not frontier:
                break
        if shown == 0:
            print("  no executed static ancestor (entered via vtable/function pointer)")


if __name__ == "__main__":
    main()
