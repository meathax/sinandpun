#!/usr/bin/env python3
"""List static bl/b callers of guest functions in main.dol.
Usage: python tools/callers.py 0x800D3830 [more]"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import menu_str as M  # noqa: E402
import xref_data as X  # noqa: E402

targets = {int(x, 16) for x in sys.argv[1:]}
base, off, ln = M.a[1], M.o[1], M.l[1]
for i in range(0, ln, 4):
    w = struct.unpack_from('>I', M.d, off + i)[0]
    if (w >> 26) == 18:
        li = w & 0x03FFFFFC
        if li & 0x2000000:
            li -= 0x4000000
        t = (base + i + li) & 0xFFFFFFFF
        if t in targets:
            pc = base + i
            f = X.func_of(pc)
            print(f'{t:08X} <- {pc:08X} {"bl" if w & 1 else "b "} in {f[0]:08X} {f[1]}')
