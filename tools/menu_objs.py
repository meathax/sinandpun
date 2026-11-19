#!/usr/bin/env python3
"""Scan a mem1.bin snapshot for live ctl menu objects (vptr 0x802FE440) and print state.
Usage: python tools/menu_objs.py build/native/UserData/Logs/base_X/mem1.bin"""
import struct
import sys

d = open(sys.argv[1], 'rb').read()


def r32(a):
    return struct.unpack_from('>I', d, a - 0x80000000)[0]


def r16(a):
    return struct.unpack_from('>H', d, a - 0x80000000)[0]


pat = struct.pack('>I', 0x802FE440)
i = 0
n = 0
while True:
    i = d.find(pat, i)
    if i < 0:
        break
    if i % 4 == 0:
        a = 0x80000000 + i
        print(f'menu obj {a:08X} flags84={r32(a+0x84):08X} items={r16(a+0xAE)} cursor={r16(a+0xB0)} '
              f'handler204={r32(a+0x204):08X} parent21C={r32(a+0x21C):08X} head248={r32(a+0xF8):08X}')
        n += 1
    i += 4
print(n, 'menu objects')
print('GM ptr', hex(r32(0x804B19A0)))
