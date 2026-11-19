#!/usr/bin/env python3
"""Static caller census for SDK functions: which ones does *game code* (callers not named in
tools/sdk_name_map.txt) call directly, and which SDK-internal callers reach them.

Usage: python tools/hle_callers.py [--libs os,dvd,...] [--name REGEX] [--depth N]
For every matched SDK function prints: addr name lib status(enabled/guest) gameCallers(count, first few)
sdkCallers(names). With --reach also reports, for the matching names, the transitive game callers.
"""
import argparse
import bisect
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ap = argparse.ArgumentParser()
ap.add_argument("--libs", default="os,dvd,nand,vi,pad,kpad,wpad,sc,exi,card,mem,thp,arc,fs,ipc,esp,si,cx,tpl,db")
ap.add_argument("--name", default="")
ap.add_argument("--reach", action="store_true")
a = ap.parse_args()
libs = {x.strip() for x in a.libs.split(",")}

starts = sorted({int(l.split()[0], 16) for l in (ROOT / "projects/r2ve01/MAP.txt").read_text().splitlines()
                 if re.match(r"^[0-9a-fA-F]{8}\s", l)})
names, libof = {}, {}
for l in (ROOT / "tools/sdk_name_map.txt").read_text().splitlines():
    p = l.split("\t")
    if len(p) >= 4 and not l.startswith("#") and p[3] == "start":
        names.setdefault(int(p[0], 16), p[1])
        libof.setdefault(int(p[0], 16), p[2])
native = {int(r["address"], 16) for r in json.load(open(ROOT / "runtime/native-index.json"))["registrations"]}

d = (ROOT / "orig/R2VE01/sys/main.dol").read_bytes()
offs = struct.unpack_from(">18I", d, 0)
addrs = struct.unpack_from(">18I", d, 0x48)
lens = struct.unpack_from(">18I", d, 0x90)
edges = {}
for i in range(18):
    if not offs[i] or not lens[i] or not (0x80003000 <= addrs[i] < 0x80400000):
        continue
    t = d[offs[i]:offs[i] + lens[i]]
    for o in range(0, len(t) - 4, 4):
        w = struct.unpack_from(">I", t, o)[0]
        if (w >> 26) == 18 and (w & 2) == 0 :  # bl and tail b
            li = (w >> 2) & 0xFFFFFF
            if li & 0x800000:
                li -= 0x1000000
            edges.setdefault((addrs[i] + o + li * 4) & 0xFFFFFFFF, set()).add(addrs[i] + o)


def fn(x):
    return starts[bisect.bisect_right(starts, x) - 1]


def is_game(f):
    return f not in names


def game_reach(f, seen):
    out = set()
    stack = [f]
    while stack:
        x = stack.pop()
        if x in seen:
            continue
        seen.add(x)
        for c in {fn(s) for s in edges.get(x, ())}:
            if is_game(c):
                out.add(c)
            else:
                stack.append(c)
    return out


rx = re.compile(a.name) if a.name else None
for addr in sorted(names):
    if libof[addr].split(".")[0] not in libs:
        continue
    if rx and not rx.search(names[addr]):
        continue
    callers = {fn(s) for s in edges.get(addr, ())}
    g = sorted(c for c in callers if is_game(c))
    s = sorted(names[c] for c in callers if not is_game(c))
    status = "NATIVE" if addr in native else "guest "
    line = f"{addr:08X} {names[addr]:34s} {status} game={len(g):3d} " + " ".join(f"{c:08X}" for c in g[:4])
    if s:
        line += "  sdk:" + ",".join(s[:5])
    if a.reach:
        r = game_reach(addr, set())
        line += f"  reachGame={len(r)}"
    if g or (a.reach and s) or rx:
        print(line)
