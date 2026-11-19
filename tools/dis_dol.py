#!/usr/bin/env python3
"""Small PPC disassembler for main.dol integer code (enough to read SDK glue by eye).

Usage: python tools/dis_dol.py 0x801A4030 [count|fn]   (fn = until blr after >=1 instr, max 400)
Names bl/b targets from tools/sdk_name_map.txt. Unknown opcodes print raw words.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
d = (ROOT / "orig/R2VE01/sys/main.dol").read_bytes()
offs = struct.unpack_from(">18I", d, 0)
addrs = struct.unpack_from(">18I", d, 0x48)
lens = struct.unpack_from(">18I", d, 0x90)
names = {}
for l in (ROOT / "tools/sdk_name_map.txt").read_text().splitlines():
    p = l.split("\t")
    if len(p) >= 4 and p[3] == "start":
        names.setdefault(int(p[0], 16), p[1])


def word(a):
    for i in range(18):
        if offs[i] and addrs[i] <= a < addrs[i] + lens[i]:
            return struct.unpack_from(">I", d, offs[i] + a - addrs[i])[0]
    return None


def s16(x):
    return ((x & 0xFFFF) ^ 0x8000) - 0x8000


def one(a, w):
    op = w >> 26
    rd, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    imm = s16(w)
    if op == 18:
        li = (w >> 2) & 0xFFFFFF
        if li & 0x800000:
            li -= 0x1000000
        t = (li * 4) & 0xFFFFFFFF if w & 2 else (a + li * 4) & 0xFFFFFFFF
        return f"{'bl' if w & 1 else 'b'} {t:08X} {names.get(t, '')}"
    if op == 16:
        t = (a + s16(w & 0xFFFC)) & 0xFFFFFFFF
        return f"bc {rd},{ra} -> {t:08X}{' lk' if w & 1 else ''}"
    if op == 19:
        xo = (w >> 1) & 0x3FF
        if xo == 16:
            return "blr" if rd == 20 else f"bclr {rd},{ra}"
        if xo == 528:
            return "bctr" if rd == 20 else f"bcctr {rd},{ra}"
        return f"op19 xo={xo}"
    tab = {32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu", 40: "lhz", 41: "lhzu",
           42: "lha", 44: "sth", 45: "sthu", 46: "lmw", 47: "stmw", 48: "lfs", 50: "lfd", 52: "stfs", 54: "stfd"}
    if op in tab:
        return f"{tab[op]} r{rd},{imm}(r{ra})"
    if op == 14:
        return f"li r{rd},{imm}" if ra == 0 else f"addi r{rd},r{ra},{imm}"
    if op == 15:
        return f"lis r{rd},0x{w & 0xFFFF:X}" if ra == 0 else f"addis r{rd},r{ra},0x{w & 0xFFFF:X}"
    if op == 11:
        return f"cmpwi cr{rd >> 2},r{ra},{imm}"
    if op == 10:
        return f"cmplwi cr{rd >> 2},r{ra},{w & 0xFFFF}"
    if op == 24:
        return "nop" if w == 0x60000000 else f"ori r{ra},r{rd},0x{w & 0xFFFF:X}"
    if op == 25:
        return f"oris r{ra},r{rd},0x{w & 0xFFFF:X}"
    if op == 28:
        return f"andi. r{ra},r{rd},0x{w & 0xFFFF:X}"
    if op == 21:
        return f"rlwinm r{ra},r{rd},{rb},{(w >> 6) & 31},{(w >> 1) & 31}"
    if op == 31:
        xo = (w >> 1) & 0x3FF
        m = {444: "or", 266: "add", 40: "subf", 0: "cmpw", 32: "cmplw", 339: "mfspr", 467: "mtspr", 23: "lwzx", 151: "stwx", 28: "and", 235: "mullw", 491: "divwu"}
        if xo == 444 and rd == rb:
            return f"mr r{ra},r{rd}"
        return f"{m.get(xo, 'op31 xo=%d' % xo)} r{rd},r{ra},r{rb}"
    return f"op{op} {w:08X}"


a = int(sys.argv[1], 16)
mode = sys.argv[2] if len(sys.argv) > 2 else "fn"
n = 400 if mode == "fn" else int(mode)
for k in range(n):
    w = word(a + 4 * k)
    if w is None:
        break
    txt = one(a + 4 * k, w)
    print(f"{a + 4 * k:08X}: {w:08X}  {txt}")
    if mode == "fn" and txt == "blr" and k > 0 and word(a + 4 * k + 4) != 0x60000000:
        # stop at the first blr that is followed by a new prologue-looking instruction
        nxt = word(a + 4 * k + 4)
        if nxt is not None and (nxt >> 26) == 37:
            break
