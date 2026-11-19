"""Symbolize build/native/calltrace.txt (written by SIN_CALLTRACE=1).

Each line of calltrace.txt is "<sequence> <guest address>": every guest call target the first time
it is called, in execution order. The tail shows where execution last made progress.

Usage:
  python tools/calltrace_sym.py --tail 60            # last 60 new call targets
  python tools/calltrace_sym.py --grep "DVD|Read"     # only SDK-named targets matching a regex
  python tools/calltrace_sym.py --from 500 --to 560   # a window by sequence number
  python tools/calltrace_sym.py --diff old.txt        # targets present now but not in old.txt
Names come from tools/sdk_name_map.txt (SDK functions byte-matched to this game); engine functions
are unnamed (shown blank) - name them by reading generated/build_shards/**/func_XXXXXXXX.cpp.
"""
import argparse
import re

ap = argparse.ArgumentParser()
ap.add_argument("--file", default="build/native/calltrace.txt")
ap.add_argument("--tail", type=int, default=0)
ap.add_argument("--grep", default=None)
ap.add_argument("--from", dest="lo", type=int, default=0)
ap.add_argument("--to", dest="hi", type=int, default=1 << 30)
ap.add_argument("--diff", default=None)
a = ap.parse_args()

names = {}
for line in open("tools/sdk_name_map.txt", encoding="utf8", errors="ignore"):
    f = line.rstrip("\n").split("\t")
    if len(f) >= 2 and re.fullmatch(r"[0-9A-Fa-f]{8}", f[0]):
        names[int(f[0], 16)] = f[1]

rows = []
for line in open(a.file):
    parts = line.split()
    if len(parts) == 2:
        rows.append((int(parts[0]), int(parts[1], 16)))

if a.diff:
    old = {int(l.split()[1], 16) for l in open(a.diff) if len(l.split()) == 2}
    rows = [r for r in rows if r[1] not in old]
if a.tail:
    rows = rows[-a.tail:]
for seq, addr in rows:
    if not (a.lo <= seq <= a.hi):
        continue
    nm = names.get(addr, "")
    if a.grep and not re.search(a.grep, nm):
        continue
    print(f"{seq:6d} 0x{addr:08X} {nm}")
