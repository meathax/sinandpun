#!/usr/bin/env python3
"""Dump printable strings of a main.dol file-offset range with their guest addresses.
Usage: python tools/menu_str.py 0x2d7b00 0x2d7e00   (DOL file offsets)
Helpers: off2addr / addr2off / word(addr)."""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
d = (ROOT / 'orig/R2VE01/sys/main.dol').read_bytes()
o = struct.unpack_from('>18I', d, 0)
a = struct.unpack_from('>18I', d, 0x48)
l = struct.unpack_from('>18I', d, 0x90)


def off2addr(x):
    for i in range(18):
        if l[i] and o[i] <= x < o[i] + l[i]:
            return a[i] + x - o[i]


def addr2off(x):
    for i in range(18):
        if l[i] and a[i] <= x < a[i] + l[i]:
            return o[i] + x - a[i]


def word(x):
    f = addr2off(x)
    return None if f is None else struct.unpack_from('>I', d, f)[0]


if __name__ == '__main__':
    lo = int(sys.argv[1], 16)
    hi = int(sys.argv[2], 16)
    for m in re.finditer(rb'[ -~]{3,}', d[lo:hi]):
        print(hex(off2addr(lo + m.start())), hex(lo + m.start()), m.group().decode())
