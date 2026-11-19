#!/usr/bin/env python3
"""List the static callees of a guest function in code order, marking which ran (calltrace).

Usage: python tools/callees.py 0x8003D9C8 [--calltrace build/native/calltrace.txt]
"""
import argparse
import bisect
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("func")
    ap.add_argument("--calltrace", default=str(ROOT / "build/native/calltrace.txt"))
    a = ap.parse_args()
    starts = sorted({int(l.split()[0], 16) for l in (ROOT / "projects/r2ve01/MAP.txt").read_text().splitlines()
                     if l.strip() and re.match(r"^[0-9a-fA-F]{8}\s", l)})
    names = {}
    for l in (ROOT / "tools/sdk_name_map.txt").read_text().splitlines():
        p = l.split("\t")
        if len(p) >= 2:
            names[int(p[0], 16)] = p[1]
    seen = {int(l.split()[1], 16) for l in open(a.calltrace) if l.strip()}
    f = int(a.func, 16)
    i = bisect.bisect_right(starts, f) - 1
    start = starts[i]
    end = starts[i + 1] if i + 1 < len(starts) else start + 0x400
    d = (ROOT / "orig/R2VE01/sys/main.dol").read_bytes()
    offs = struct.unpack_from(">18I", d, 0)
    addrs = struct.unpack_from(">18I", d, 0x48)
    lens = struct.unpack_from(">18I", d, 0x90)
    for k in range(18):
        if offs[k] and lens[k] and addrs[k] <= start < addrs[k] + lens[k]:
            t = d[offs[k]:offs[k] + lens[k]]
            base = addrs[k]
            break
    print(f"fn_{start:08X}..{end:08X}")
    for addr in range(start, end, 4):
        w = struct.unpack_from(">I", t, addr - base)[0]
        if (w >> 26) == 18 and (w & 3) == 1:
            li = (w >> 2) & 0xFFFFFF
            if li & 0x800000:
                li -= 0x1000000
            tgt = (addr + li * 4) & 0xFFFFFFFF
            tstart = starts[bisect.bisect_right(starts, tgt) - 1]
            flag = "RAN " if tgt in seen else "    "
            print(f"  {addr:08X}: bl {tgt:08X} {flag}{names.get(tgt, '')}")


if __name__ == "__main__":
    main()
