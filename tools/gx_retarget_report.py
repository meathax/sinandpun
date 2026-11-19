#!/usr/bin/env python3
"""Report disabled (donor-address) HLE functions that the game actually calls, by SDK name.

For every SIN_DISABLED_NATIVE_OVERRIDE* in runtime/src, derive the SDK name from the C++ symbol
(GX__SetVtxDesc_8016xxxx -> GXSetVtxDesc), look it up in tools/sdk_name_map.txt (Sin address) and
check build/native/calltrace.txt (SIN_CALLTRACE=1 run). Lists candidates to retarget: disabled
HLE + Sin function exists + the game calls it + no enabled native covers that address.

Usage: python tools/gx_retarget_report.py [--all]
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

pat = re.compile(r"SIN_DISABLED_NATIVE_OVERRIDE(?:_VOID)?\(\s*([0-9A-Fa-f]{8})\s*,\s*(\w+)")
disabled = []
for f in (ROOT / "runtime/src").rglob("*.cpp"):
    for m in pat.finditer(f.read_text(errors="replace")):
        disabled.append((f.relative_to(ROOT / "runtime/src").as_posix(), m.group(1).upper(), m.group(2)))

names = {}
for line in (ROOT / "tools/sdk_name_map.txt").read_text().splitlines():
    p = line.split("\t")
    if len(p) >= 4 and p[3] in ("start", "interior"):
        names.setdefault(p[1], []).append(int(p[0], 16))

native = {int(r["address"], 16) for r in json.load(open(ROOT / "runtime/native-index.json"))["registrations"]}
seen_path = ROOT / "build/native/calltrace.txt"
seen = {int(l.split()[1], 16) for l in seen_path.read_text().splitlines() if l.strip()} if seen_path.exists() else set()


def sdk_name(sym):
    s = re.sub(r"_[0-9a-fA-F]{8}$", "", sym)
    s = re.sub(r"^(GX)__", r"\1", s)
    s = re.sub(r"^(OS|VI|AI|AX|DVD|NAND|IPC|SC|WPAD|KPAD|PAD|ARC)__", r"\1", s)
    s = re.sub(r"_(HLE|hle)$", "", s)
    return s


show_all = "--all" in sys.argv
rows = []
for src, addr, sym in disabled:
    base = sdk_name(sym)
    for cand in (base, base.replace("GX_", "GX"), "__" + base):
        if cand in names:
            for a in names[cand]:
                rows.append((src, addr, sym, cand, a, a in native, a in seen))
            break
print(f"{len(disabled)} disabled natives; {len(rows)} map to a Sin SDK name")
print("RETARGET CANDIDATES (called by the game, no native at the Sin address):")
for src, addr, sym, name, a, isnative, called in sorted(rows, key=lambda r: r[4]):
    if called and not isnative:
        print(f"  {a:08X} {name:32s} <- {src} {sym} (donor {addr})")
if show_all:
    print("NOT YET CALLED (no evidence needed now):")
    for src, addr, sym, name, a, isnative, called in sorted(rows, key=lambda r: r[4]):
        if not called and not isnative:
            print(f"  {a:08X} {name:32s} <- {src} {sym}")
