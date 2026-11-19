#!/usr/bin/env python3
"""Find DOL callers of MsgString::get (0x80140FD4 / 0x80141078 variant) and the immediate message id (li r3,N).
Usage: python tools/msg_xref.py [id ...]   (no ids = list all)"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import menu_str as M  # noqa: E402
import xref_data as X  # noqa: E402

TARGETS = {0x80140FD4, 0x80141078}
want = {int(x) for x in sys.argv[1:]}
for sec in (0, 1):
    base, off, ln = M.a[sec], M.o[sec], M.l[sec]
    words = [struct.unpack_from('>I', M.d, off + i)[0] for i in range(0, ln, 4)]
    for i, w in enumerate(words):
        if (w >> 26) == 18 and (w & 1):  # bl
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            tgt = (base + 4 * i + li) & 0xFFFFFFFF
            if tgt in TARGETS:
                val = None
                for j in range(i - 1, max(i - 8, -1), -1):
                    v = words[j]
                    if (v >> 26) == 14 and ((v >> 21) & 31) == 3 and ((v >> 16) & 31) == 0:
                        val = v & 0xFFFF
                        break
                if not want or val in want:
                    pc = base + 4 * i
                    f = X.func_of(pc)
                    print(f'{pc:08X} id={val} in {f[0]:08X} {f[1]}')
