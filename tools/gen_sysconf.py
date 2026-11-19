#!/usr/bin/env python3
"""Synthesize a valid retail NTSC-U SYSCONF for the managed NAND.

Format per WiiBrew:
  header (0x20): 'SCv0' | u32(3) | u32(size BE) | 0xFF*20
  entries: {u8 type; char[2] name; u8 flags; u16 length BE; data}
  footer (0x100): SHA-1(file[:-0x100]) + 0xFF padding

NET.WCFG = 0 (wifi disabled) is the true state of an offline retail console:
NWC24's boot path takes the 'no wireless configuration' branch instead of
registering network shutdown machinery it cannot validate without IOS.
"""
import hashlib
import struct
import sys
from pathlib import Path

OUT = Path(sys.argv[1] if len(sys.argv) > 1 else
           r"D:/Arcade/AI/aCORES/Sin/build/native/UserData/NAND/shared2/sys/SYSCONF")

# (type, name, length, data) - types: 0 bigarray, 1 byte, 2 short, 3 long,
# 4 longlong, 5 bool(1), 6 bool2(2)
ENTRIES = [
    (1, b"IPL", b"LNG", 1, bytes([1])),           # English
    (0, b"IPL", b"SADR", 0x1008, b"\x00\x02" + b"\x00" * 0x1006),  # USA locale
    (5, b"IPL", b"E60", 1, bytes([1])),           # 60Hz supported
    (0, b"IPL", b"IDL", 0x460, b"\x00" * 0x460),  # idle screen (blank)
    (1, b"IPL", b"AR", 1, bytes([0])),           # 4:3 aspect
    (5, b"IPL", b"SSV", 1, bytes([0])),          # screensaver off
    (5, b"IPL", b"SC", 1, bytes([0])),           # soft reset enabled? retail 0
    (1, b"IPL", b"CD", 1, bytes([0])),           # no clock display
    (1, b"IPL", b"CB", 1, bytes([0])),           # brightness mid
    (1, b"BT", b"SENS", 1, bytes([3])),          # sensor bar sensitivity
    (1, b"BT", b"SPKV", 1, bytes([0x58])),       # speaker volume
    (5, b"BT", b"MOT", 1, bytes([1])),           # wiimote motor on
    (1, b"BT", b"BAR", 1, bytes([1])),           # sensor bar below
    (5, b"NET", b"WCFG", 1, bytes([0])),         # WIFI DISABLED (offline retail)
    (0, b"NET", b"CFG", 0x1AA4, b"\x00" * 0x1AA4),
    (3, b"UTC", b"UTC", 4, struct.pack(">i", 0)),  # offset 0
    (1, b"WWW", b"RST", 1, bytes([0])),
    (1, b"MPL", b"VOL", 1, bytes([0])),
    (1, b"MPL", b"CDI", 1, bytes([0])),
    (5, b"DEV", b"VCT", 1, bytes([0])),
]


def build():
    body = bytearray()
    for typ, group, name, length, data in ENTRIES:
        assert len(data) == length, (name, length, len(data))
        body.append(typ)
        body += group + name
        body.append(0xFF)  # flags
        body += struct.pack(">H", length)
        body += data
    total = 0x20 + len(body) + 0x100
    out = bytearray()
    out += b"SCv0"
    out += struct.pack(">I", 3)
    out += struct.pack(">I", total)
    out += b"\xFF" * 20
    out += body
    sha = hashlib.sha1(bytes(out)).digest()
    out += sha + b"\xFF" * (0x100 - len(sha))
    assert len(out) == total
    return bytes(out)


def main():
    blob = build()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(blob)
    print(f"wrote {OUT} ({len(blob)} bytes, sha1 {hashlib.sha1(blob).hexdigest()})")


if __name__ == "__main__":
    sys.exit(main())
