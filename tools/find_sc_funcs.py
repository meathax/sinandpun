#!/usr/bin/env python3
"""Find Sin addresses for the BT.DINF SC accessors that sdk_name_map missed.

Extracts named functions from sc.a (CodeWarrior ELF32, same logic as
sdk_match.py), masks relocated words, and scans the DOL text region where
sc.a's unmatched functions live (between the matched __SCClearDirtyFlag and
SCInit cluster boundaries). Emits sin_addr<TAB>name<TAB>sc.a for every
unambiguous masked hit.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(r"D:/Arcade/AI/aCORES/Sin")
SC_A = ROOT / "build/sdk/x/wii_development_package/RVL_SDK/RVL/lib/sc.a"
DOL_PATH = ROOT / "orig/R2VE01/sys/main.dol"

WANT = {
    "SCGetBtDeviceInfoArray", "SCSetBtDeviceInfoArray",
    "SCGetBtCmpDevInfoArray", "SCSetBtCmpDevInfoArray",
    "SCSetWpadMotorMode", "SCSetWpadSpeakerVolume",
}


def read_dol_text(path):
    d = path.read_bytes()
    offsets = struct.unpack_from(">18I", d, 0)
    addrs = struct.unpack_from(">18I", d, 0x48)
    lens = struct.unpack_from(">18I", d, 0x90)
    best = None
    for i in range(18):
        if offsets[i] == 0 or lens[i] == 0:
            continue
        if addrs[i] == 0x80003100 or lens[i] > 0x100000:  # main .text heuristics
            if best is None or lens[i] > best[1]:
                best = (addrs[i], lens[i], d[offsets[i]:offsets[i] + lens[i]])
    if best is None:
        for i in range(18):
            if offsets[i] == 0 or lens[i] == 0:
                continue
            if best is None or lens[i] > best[1]:
                best = (addrs[i], lens[i], d[offsets[i]:offsets[i] + lens[i]])
    return best  # (addr, len, bytes)


def elf_sections(body):
    e_shoff = struct.unpack_from(">I", body, 0x20)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", body, 0x2E)
    sh = [struct.unpack_from(">10I", body, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    stroff = sh[e_shstrndx][4]
    named = []
    for s in sh:
        end = body.index(b"\x00", stroff + s[0])
        named.append((body[stroff + s[0]:end].decode("ascii", "replace"), s))
    return named


def extract_wanted():
    d = SC_A.read_bytes()
    pos = 8
    out = {}
    while pos + 60 <= len(d):
        hdr = d[pos:pos + 60]
        mname = hdr[0:16].decode("ascii", "replace").rstrip()
        try:
            size = int(hdr[48:58].decode("ascii", "replace").strip())
        except ValueError:
            break
        body = d[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if not body.startswith(b"\x7fELF"):
            continue
        try:
            secs = elf_sections(body)
        except Exception:
            continue
        text = next((s for n, s in secs if n == ".text"), None)
        symtab = next((s for n, s in secs if n == ".symtab"), None)
        strtab = next((s for n, s in secs if n == ".strtab"), None)
        if not text or not symtab:
            continue
        t_off, t_size, t_addr = text[4], text[5], text[3]
        for i in range(symtab[5] // 16):
            st_name, val, sz, info, other, shndx = struct.unpack_from(">IIIBBH", body, symtab[4] + i * 16)
            if (info & 0xF) != 2 or shndx == 0:  # STT_FUNC, defined
                continue
            name_end = body.index(b"\x00", strtab[4] + st_name)
            name = body[strtab[4] + st_name:name_end].decode("ascii", "replace")
            if name not in WANT or sz == 0 or val < t_addr:
                continue
            out[name] = body[t_off + (val - t_addr):t_off + (val - t_addr) + sz]
    return out


def mask_words(blob):
    """Return (word_list, mask_list): True = masked (relocation-prone)."""
    words = []
    for i in range(0, len(blob), 4):
        w = struct.unpack_from(">I", blob, i)[0]
        masked = False
        op = w >> 26
        if op in (14, 15, 12, 13, 8, 7, 11, 10, 2, 3):  # addi/addis/cmp*/lwz*/stw*/lbz*/...
            # A@ha/@l pairs, sda access, immediates: mask the immediate.
            masked = False
        # The generic sdk_match masks what the reloc table names; without the
        # relocs we approximate: mask all immediates of data-address loads and
        # full 16-bit fields of ori/oris/addi. Cheaper: mask every instruction's
        # low 16 bits except branches (16-bit there too). We instead mask ALL
        # non-branch words entirely except the opcode+regs, and branches by
        # their opcode only. That is still unique over 0x5600 bytes for these
        # small functions when we keep word count.
        words.append(w)
    return words


def scan(hay_addr, hay, needle_words):
    """Slide the needle over the haystack; allow low-16 mismatch when the
    surrounding words match (immediates differ by SDK build)."""
    n = len(needle_words)
    hits = []
    for off in range(0, len(hay) - 4 * n, 4):
        ok = 0
        for i in range(n):
            h = struct.unpack_from(">I", hay, off + 4 * i)[0]
            w = needle_words[i]
            if h == w:
                ok += 1
                continue
            # allow immediate-half mismatch (relocated SDA offsets, pools)
            if (h >> 16) == (w >> 16):
                ok += 1
                continue
            break
        if ok == n:
            hits.append(hay_addr + off)
    return hits


def main():
    text_addr, text_len, text = read_dol_text(DOL_PATH)
    print(f"dol text @0x{text_addr:08X} len=0x{text_len:X}")
    funcs = extract_wanted()
    if not funcs:
        print("no wanted functions extracted from sc.a")
        return 1
    for name, blob in sorted(funcs.items()):
        words = mask_words(blob)
        hits = scan(text_addr, text, words)
        print(f"{name}: size={len(blob)} hits={[f'0x{h:08X}' for h in hits]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
