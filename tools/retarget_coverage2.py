#!/usr/bin/env python3
"""Measure retarget coverage with CW demangling + prefix normalization.

Bridges donor (Dolphin-style demangled) names to SDK .a symbol names:
- SDK API prefixes stripped (OSSendMessage <-> OS::SendMessage)
- CW-mangled nw4r names demangled (SetVolume__Q28nw4r3snd11SoundPlayerFf
  <-> nw4r::snd::SoundPlayer::SetVolume)
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(r"D:/Arcade/AI/aCORES/Sin")
MAP_LINE = re.compile(r"^([0-9A-Fa-f]{8})\s+(\S+)\s*$")

LIB_PREFIXES = (
    "os", "gx", "vi", "ai", "ax", "axfx", "axart", "si", "pad", "dvd", "dsp",
    "thp", "kpad", "wpad", "wbc", "exi", "nand", "sc", "mtx", "arc", "base",
    "mem", "fs", "ipc", "cx", "esp", "enc", "gd", "hid", "kbd", "kpr", "fnt",
    "cnt", "mix", "syn", "seq", "midi", "usb", "hio2", "perf", "euart", "rso",
    "db", "sp", "tpl", "demo", "darch", "oslog", "g2d", "sample", "wenc", "pmic",
)


def cw_demangle(name: str):
    """Best-effort CodeWarrior demangle -> 'ns::Class::method' form (args dropped)."""
    m = re.match(r"^(__ct__|__dt__|__nw|__dl)?(.+?)__((?:Q|R)\d+.*)$", name)
    if not m:
        return None
    special, method, tail = m.groups()
    chain = []
    if tail[0] in "QR":
        depth = int(re.match(r"[QR](\d+)", tail).group(1))
        rest = re.sub(r"^[QR]\d+", "", tail)
        for _ in range(depth):
            lm = re.match(r"^(\d+)", rest)
            if not lm:
                return None
            ln = int(lm.group(1))
            chain.append(rest[len(lm.group(0)):len(lm.group(0)) + ln])
            rest = rest[len(lm.group(0)) + ln:]
    if special:
        method = {"__ct__": chain[-1] if chain else method, "__dt__": ("~" + chain[-1]) if chain else method}.get(
            special, method)
    full = "::".join(chain + [method])
    return full


def norm(name: str) -> str:
    n = name.strip()
    # CW-mangled? demangle first.
    dm = cw_demangle(n)
    if dm:
        n = dm
    n = re.sub(r"^(RVL::|OS::|GX::|DVD::|VI::|DSP::|SC::)", "", n)
    n = n.replace("::", "_")
    n = n.replace("__", "_")
    n = re.sub(r"_HLE(_[0-9A-Fa-f]{8})?$", "", n)
    n = re.sub(r"_80[0-9a-f]{6}$", "", n)
    n = re.sub(r"^__", "", n)
    n = n.strip("_")
    low = n.lower()
    # Strip one known library prefix (OSSendMessage -> SendMessage).
    for p in LIB_PREFIXES:
        if low.startswith(p) and len(low) > len(p):
            nxt = n[len(p):]
            if nxt and (nxt[0].isupper() or nxt[0] == "_"):
                low = nxt.strip("_").lower()
                break
    return low


def main():
    sdk = {}
    for line in (ROOT / "tools/sdk_name_map.txt").read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        addr, name, lib = line.split("\t")[:3]
        sdk.setdefault(norm(name), (addr, name, lib))

    donor = {}
    for line in (ROOT / "reference/Wiicompiled/projects/mkwii/MAP.txt").read_text(
            encoding="utf-8", errors="replace").splitlines():
        m = MAP_LINE.match(line.strip())
        if m:
            donor.setdefault(int(m.group(1), 16), m.group(2))

    index = json.loads((ROOT / "runtime/native-index.json").read_text())
    regs = {int(r["address"], 16): r["symbol"] for r in index["registrations"]}

    pat = re.compile(r"MKW_GADDR(?:_CT)?\(([0-9A-Fa-f]{8})\)")
    gaddr = {}
    for p in list((ROOT / "runtime/src").rglob("*.cpp")) + list((ROOT / "runtime/src").rglob("*.h")):
        for m in pat.finditer(p.read_text(encoding="utf-8", errors="replace")):
            gaddr.setdefault(int(m.group(1), 16), str(p.relative_to(ROOT / "runtime")))

    out = {}
    for label, table in (("registrations", regs), ("gaddr", gaddr)):
        ok, miss, unnamed = [], [], []
        for a in table:
            name = donor.get(a)
            if not name:
                unnamed.append((a, table[a]))
                continue
            hit = sdk.get(norm(name))
            if hit:
                ok.append((a, name, hit[0]))
            else:
                miss.append((a, name))
        out[label] = (ok, miss, unnamed)
        print(f"{label}: {len(table)} needed, {len(ok)} resolved, "
              f"{len(miss)} unmatched, {len(unnamed)} unnamed-in-donor")
        for a, n in miss[:15]:
            print(f"    miss {a:08X} {n}")

    (ROOT / "tools/retarget_resolved.json").write_text(json.dumps({
        label: [f"{a:08X}\t{n}\t{sin}" for a, n, sin in v[0]] for label, v in out.items()}, indent=1))
    print("\nwrote tools/retarget_resolved.json")
    print(f"SDK map: {len(sdk)} normalized names")


if __name__ == "__main__":
    sys.exit(main())
