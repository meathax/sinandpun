#!/usr/bin/env python3
"""Find code that materialises (lis+addi/ori) or stores (data pointer) a guest data address.
Usage: python tools/xref_data.py 0x802dbb54 [more addrs]   -> function containing each reference (via MAP.txt)."""
import bisect
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import menu_str as M  # noqa: E402

ROOT = M.ROOT
starts = []
names = {}
for line in (ROOT / 'projects/r2ve01/MAP.txt').read_text().splitlines():
    p = line.split()
    if len(p) >= 2 and re.match(r'^[0-9a-fA-F]{8}$', p[0]):
        starts.append(int(p[0], 16))
        names[int(p[0], 16)] = p[1]
starts = sorted(set(starts))


def func_of(addr):
    i = bisect.bisect_right(starts, addr) - 1
    return (starts[i], names.get(starts[i], '?')) if i >= 0 else (0, '?')


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


def scan(targets):
    res = {t: [] for t in targets}
    for sec in range(18):
        if not M.l[sec] or sec > 6:
            pass
    # text sections 0,1
    for sec in (0, 1):
        base, off, ln = M.a[sec], M.o[sec], M.l[sec]
        lis = {}
        for i in range(0, ln, 4):
            w = struct.unpack_from('>I', M.d, off + i)[0]
            op = w >> 26
            rd = (w >> 21) & 31
            ra = (w >> 16) & 31
            pc = base + i
            if op == 15 and ra == 0:
                lis[rd] = (w & 0xFFFF) << 16
            elif op == 14 and ra in lis:
                v = (lis[ra] + s16(w & 0xFFFF)) & 0xFFFFFFFF
                if v in res:
                    res[v].append(pc)
            elif op == 24 and ra in lis:  # ori rA(dst)=bits 16..20 is rA, source rS=21..25
                pass
            if op == 24:  # ori ra,rs,imm : rs = rd field
                if rd in lis:
                    v = (lis[rd] | (w & 0xFFFF)) & 0xFFFFFFFF
                    if v in res:
                        res[v].append(pc)
    # data pointers
    for sec in range(18):
        if not M.l[sec]:
            continue
        for i in range(0, M.l[sec], 4):
            w = struct.unpack_from('>I', M.d, M.o[sec] + i)[0]
            if w in res and sec > 1:
                res[w].append(('data', M.a[sec] + i))
    return res


if __name__ == '__main__':
    tg = [int(x, 16) for x in sys.argv[1:]]
    r = scan(tg)
    for t, refs in r.items():
        print(hex(t))
        for x in refs:
            if isinstance(x, tuple):
                print('   data ptr at', hex(x[1]))
            else:
                f, n = func_of(x)
                print('   code', hex(x), 'in', hex(f), n)
