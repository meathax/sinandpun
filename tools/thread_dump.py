"""Dump guest OS thread state and scheduler globals from a crash/watchdog artifact.

Usage: python tools/thread_dump.py build/native/UserData/Logs/base_XXXX   (dir or the mem1.bin path)
Guest memory is BIG-ENDIAN, MEM1 base 0x80000000.
OSThread layout: OSContext 0x000-0x2C7 (r1 at +4, LR +0x84, SRR0 +0x198), state u16 +0x2C8
(1 ready, 2 running, 4 waiting, 8 moribund), priority +0x2D0, wait queue +0x2DC, active-list next +0x2FC.
Scheduler globals (Sin, retargeted): RunQueueBits 0x804B22A8 (bit 0 = SDK idle thread, always set),
RunQueueHint 0x804B22A4, Reschedule 0x804B22A0.
"""
import os
import struct
import sys

p = sys.argv[1]
if os.path.isdir(p):
    p = os.path.join(p, "mem1.bin")
m = open(p, "rb").read()


def r32(a):
    return struct.unpack(">I", m[a - 0x80000000:a - 0x80000000 + 4])[0]


def r16(a):
    return struct.unpack(">H", m[a - 0x80000000:a - 0x80000000 + 2])[0]


print("active-thread head @0x800000DC =", hex(r32(0x800000DC)), " current ctx @0x800000D4 =", hex(r32(0x800000D4)),
      " running @0x800000E4 =", hex(r32(0x800000E4)))
t, n = r32(0x800000DC), 0
while t and n < 64:
    print(f"{t:#010x} state={r16(t + 0x2C8)} pri={r32(t + 0x2D0)} susp={r32(t + 0x2CC)} srr0={r32(t + 0x198):#x} "
          f"lr={r32(t + 0x84):#x} sp={r32(t + 4):#x} waitqueue={r32(t + 0x2DC):#x}")
    t = r32(t + 0x2FC)
    n += 1
print(f"RunQueueBits={r32(0x804B22A8):#x} RunQueueHint={r32(0x804B22A4):#x} Reschedule={r32(0x804B22A0):#x}")
