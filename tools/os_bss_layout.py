import struct
import sys
from pathlib import Path

d = Path(r"D:/Arcade/AI/aCORES/Sin/build/sdk/x/wii_development_package/RVL_SDK/RVL/lib/os.a").read_bytes()
pos = 8
while pos + 60 <= len(d):
    hdr = d[pos:pos + 60]
    mname = hdr[0:16].decode("ascii", "replace").rstrip()
    try:
        size = int(hdr[48:58].decode("ascii", "replace").strip())
    except ValueError:
        break
    body = d[pos + 60:pos + 60 + size]
    pos += 60 + size + (size & 1)
    if mname.startswith("/") or not body.startswith(b"\x7fELF"):
        continue
    e_shoff = struct.unpack_from(">I", body, 0x20)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", body, 0x2E)
    sh = [struct.unpack_from(">10I", body, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    stroff = sh[e_shstrndx][4]
    names = []
    for s in sh:
        end = body.index(b"\x00", stroff + s[0])
        names.append(body[stroff + s[0]:end].decode("ascii", "replace"))
    symtab = next((s for n, s in zip(names, sh) if n == ".symtab"), None)
    strtab = sh[symtab[6]] if symtab else None
    if symtab is None:
        continue
    rows = []
    for i in range(symtab[5] // 16):
        st_name, st_value, st_size, st_info, _o, st_shndx = \
            struct.unpack_from(">IIIBBH", body, symtab[4] + i * 16)
        sec = names[st_shndx] if st_shndx < len(names) else ""
        nm = body[strtab[4] + st_name:body.index(b"\x00", strtab[4] + st_name)].decode("ascii", "replace")
        if sec in (".bss", ".sbss") and nm in ("RunQueue", "RunQueueBits", "RunQueueHint", "DefaultThread"):
            rows.append((nm, st_value, st_size, sec))
    if rows:
        for r in sorted(rows):
            print(f"{r[0]:14s} value=0x{r[1]:X} size=0x{r[2]:X} ({r[3]})")
        break
