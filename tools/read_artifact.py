import struct
import sys
from pathlib import Path

d = sorted(Path(r"D:/Arcade/AI/aCORES/Sin/build/native/UserData/Logs").glob("base_*"),
           key=lambda p: p.stat().st_mtime)[-1]
print("artifact:", d.name)
m = (d / "mem1.bin").read_bytes()
for a in (0x804B21D8, 0x804B21DC, 0x8000004C, 0x8000315C, 0x804B5A18):
    print(f"*(0x{a:08X}) = 0x{struct.unpack_from('>I', m, a - 0x80000000)[0]:08X}")
