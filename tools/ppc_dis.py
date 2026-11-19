#!/usr/bin/env python3
"""Minimal PPC disassembler for targeted DOL .text ranges (boot assert tracing)."""
import struct
import sys
from pathlib import Path

DOL = Path(r"D:/Arcade/AI/aCORES/Sin/orig/R2VE01/sys/main.dol")


def load_text():
    d = DOL.read_bytes()
    offs = struct.unpack_from(">18I", d, 0)
    addrs = struct.unpack_from(">18I", d, 0x48)
    lens = struct.unpack_from(">18I", d, 0x90)
    for i in range(18):
        if addrs[i] == 0x8001F240:
            return addrs[i], d[offs[i]:offs[i] + lens[i]]
    raise RuntimeError


def dis(w, pc):
    op = w >> 26
    a = (w >> 16) & 0x1F
    b = (w >> 11) & 0x1F
    c = (w >> 6) & 0x1F
    d = w & 0xFFFF
    sd = d - 0x10000 if d >= 0x8000 else d
    if op == 14:
        return f"addi r{a}, r{b}, {sd}"
    if op == 12:
        return f"addic r{a}, r{b}, {sd}"
    if op == 15:
        return f"lis r{a}, {d & 0xFFFF:#x}"
    if op == 24:
        return f"ori r{a}, r{b}, {d:#x}"
    if op == 25:
        return f"oris r{a}, r{b}, {d:#x}"
    if op == 32:
        return f"lwz r{a}, {sd}(r{b})"
    if op == 34:
        return f"lbz r{a}, {sd}(r{b})"
    if op == 36:
        return f"stw r{a}, {sd}(r{b})"
    if op == 38:
        return f"stb r{a}, {sd}(r{b})"
    if op == 40:
        return f"lhz r{a}, {sd}(r{b})"
    if op == 44:
        return f"sth r{a}, {sd}(r{b})"
    if op == 47:
        return f"lmw r{a}, {sd}(r{b})"
    if op == 11:
        target = w & 0x03FFFFFC
        if target & 0x02000000:
            target -= 0x04000000
        return f"cmpwi cr{c}, r{a}, {sd}"
    if op == 10:
        return f"cmplwi cr{c}, r{a}, {d:#x}"
    if op == 32 + 0:  # unreachable guard
        pass
    if (w & 0xFC0007FE) == 0x7C000000:
        return "cmp"
    if (w >> 1) == (0x7C000000 >> 1) & 0x7C000000:
        pass
    if w == 0x4E800020:
        return "blr"
    if op == 19:
        xo = (w >> 1) & 0x3FF
        bo = a
        bi = b
        target = w & 0xFFFC
        if target & 0x8000:
            target -= 0x10000
        if xo == 16:
            return f"b{bo}{bi} 0x{pc + target:08X}"
        if xo == 528:
            return f"bcctr (bo={bo} bi={bi})"
        if xo == 0:
            return "mcrf"
        return f"op19 xo={xo}"
    if op == 18:
        target = w & 0x03FFFFFC
        aa = w & 2
        lk = w & 1
        if aa:
            return f"b 0x{target:08X}" + (" (lk)" if lk else "")
        return f"b 0x{(pc + (target if not (target & 0x02000000) else target - 0x04000000)):08X}" + (" (lk)" if lk else "")
    if op == 16:
        target = w & 0xFFFC
        if target & 0x8000:
            target -= 0x10000
        return f"bc 0x{pc + target:08X} (bo={a} bi={b})"
    if op == 21:
        m = (w >> 1) & 0x3FF
        return f"rlwinm r{a}, r{b}, {c}, {(w >> 6) & 0x1F}"
    if op == 20:
        return f"rlwimi r{a}, r{b}, {c}"
    if op == 31:
        xo = (w >> 1) & 0x3FF
        known = {266: "add", 40: "subf", 28: "and", 444: "or", 60: "andc",
                 836: "srw", 536: "srw", 792: "sraw", 24: "slw", 824: "sraw",
                 235: "mullw", 233: "mulhw", 26: "cntlzw", 922: "extsh", 954: "extsb"}
        return f"op31 {known.get(xo, xo)} r{a}, r{b}, r{c}"
    if op == 13:
        return f"addic. r{a}, r{b}, {sd}"
    if op == 8:
        return "subfic"
    if op == 7:
        return f"mulli r{a}, r{b}, {sd}"
    return f"op{op} {w:08X}"


def main():
    lo = int(sys.argv[1], 16)
    hi = int(sys.argv[2], 16)
    base, text = load_text()
    for pc in range(lo, hi, 4):
        w = struct.unpack_from(">I", text, pc - base)[0]
        print(f"{pc:08X}  {w:08X}  {dis(w, pc)}")


if __name__ == "__main__":
    sys.exit(main())
