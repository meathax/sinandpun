#!/usr/bin/env python3
"""HLE audit report for the non-GX / non-audio SDK surface.

For every SDK function in tools/sdk_name_map.txt whose library is in --libs (default: os dvd nand
vi pad kpad wpad sc exi card mem thp arc fs ipc esp ...), list:
  * ENABLED  : a PPC_NATIVE_OVERRIDE is registered at the Sin address (runtime/native-index.json)
  * DISABLED : a SIN_DISABLED_NATIVE_OVERRIDE exists whose derived SDK name matches (donor HLE not live)
  * none     : real translated guest code runs
plus (--calltrace FILE) whether the game called it in a SIN_CALLTRACE run.
With --hw it also scans translated shards for functions touching 0xCC00xxxx / 0xCD00xxxx MMIO
(lis rX,0xCC00 / 0xCD00 patterns in main.dol words) and reports those without a native.

Usage: python tools/hle_retarget_report.py [--libs os,dvd] [--calltrace build/native/calltrace.txt] [--hw] [--md]
"""
import argparse
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ap = argparse.ArgumentParser()
ap.add_argument("--libs", default="os,dvd,nand,vi,pad,kpad,wpad,wud,sc,exi,card,mem,thp,arc,fs,ipc,esp,si,ai,cx,tpl,nwc24,net,usb,bte,wenc,wbc,hbm,nlib,sync,rso,rvl")
ap.add_argument("--calltrace")
ap.add_argument("--hw", action="store_true")
args = ap.parse_args()
libs = {l.strip().lower() for l in args.libs.split(",")}

names = {}  # addr -> (name, lib, quality)
for line in (ROOT / "tools/sdk_name_map.txt").read_text().splitlines():
    p = line.split("\t")
    if len(p) >= 4 and not line.startswith("#"):
        names[int(p[0], 16)] = (p[1], p[2], p[3] + (" " + p[4] if len(p) > 4 else ""))
name2addr = {}
for a, (n, lib, q) in names.items():
    name2addr.setdefault(n, []).append(a)

idx = json.load(open(ROOT / "runtime/native-index.json"))["registrations"]
enabled = {int(r["address"], 16): (r["symbol"], r["sourceFile"]) for r in idx}

pat = re.compile(r"SIN_DISABLED_NATIVE_OVERRIDE(?:_VOID)?\(\s*([0-9A-Fa-f]{8})\s*,\s*(\w+)")
disabled = []
for f in (ROOT / "runtime/src").rglob("*.cpp"):
    for m in pat.finditer(f.read_text(errors="replace")):
        disabled.append((f.relative_to(ROOT / "runtime/src").as_posix(), m.group(1).upper(), m.group(2)))


def sdk_name(sym):
    s = re.sub(r"_[0-9a-fA-F]{8}$", "", sym)
    s = re.sub(r"_(HLE|hle)$", "", s)
    s = re.sub(r"^([A-Za-z]+?)__", r"\1", s)
    return s


dis_by_name = {}
for src, addr, sym in disabled:
    dis_by_name.setdefault(sdk_name(sym), []).append((src, addr, sym))

seen = set()
if args.calltrace and Path(args.calltrace).exists():
    seen = {int(l.split()[1], 16) for l in Path(args.calltrace).read_text().splitlines() if l.strip()}

print("addr      name                                 lib     status    detail")
for a in sorted(names):
    n, lib, q = names[a]
    libbase = lib.split(".")[0].lower()
    if libbase not in libs:
        continue
    if a in enabled:
        st, det = "ENABLED", f"{enabled[a][0]} ({enabled[a][1]})"
    else:
        cands = dis_by_name.get(n) or dis_by_name.get(n.lstrip("_"))
        if cands:
            st, det = "DISABLED", "; ".join(f"{s}:{ad}:{sy}" for s, ad, sy in cands)
        else:
            st, det = "guest", ""
    called = "" if not seen else (" CALLED" if a in seen else "")
    print(f"{a:08X}  {n:36s} {lib:7s} {st:9s} {det}{called}")

if args.hw:
    dol = ROOT / "orig/R2VE01/sys/main.dol"
    if not dol.exists():
        cand = list((ROOT / "orig").rglob("main.dol"))
        dol = cand[0] if cand else None
    if dol:
        b = dol.read_bytes()
        off = struct.unpack_from(">18I", b, 0)
        addr = struct.unpack_from(">18I", b, 0x48)
        size = struct.unpack_from(">18I", b, 0x90)
        text = []
        for i in range(18):
            if size[i] and off[i] and addr[i] < 0x80400000:
                text.append((addr[i], b[off[i]:off[i] + size[i]]))
        starts = sorted({int(l.split()[0], 16) for l in (ROOT / "projects/r2ve01/MAP.txt").read_text().splitlines() if re.match(r"^[0-9a-fA-F]{8}\s", l)})
        import bisect
        hits = {}
        for base, data in text:
            for o in range(0, len(data) - 3, 4):
                w = struct.unpack(">I", data[o:o + 4])[0]
                # addis rD,0,imm (lis) with imm 0xCC00/0xCD00 (op 15, rA=0)
                if (w >> 26) == 15 and ((w >> 16) & 31) == 0 and (w & 0xFFFF) in (0xCC00, 0xCD00, 0xCC01, 0xCD80, 0xCD01, 0xCC02, 0xCC03, 0xCD02, 0xCD04, 0xCD06, 0xCD08):
                    pc = base + o
                    i = bisect.bisect_right(starts, pc) - 1
                    fa = starts[i] if i >= 0 else 0
                    hits.setdefault(fa, []).append((pc, w & 0xFFFF))
        print("\nHARDWARE-REGISTER TOUCHERS (lis 0xCCxx/0xCDxx):")
        for fa in sorted(hits):
            n = names.get(fa, ("?", "?", ""))
            st = "ENABLED" if fa in enabled else "no-native"
            print(f"{fa:08X} {n[0]:34s} {n[1]:8s} {st:9s} lis@{','.join('%08X/%04X' % h for h in hits[fa][:4])}")
