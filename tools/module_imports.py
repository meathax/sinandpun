#!/usr/bin/env python3
"""Cross-reference DOL-resident callees of the 8 stage modules with SDK names and native overrides.

Writes docs/MODULE-IMPORTS.md. Callee sources (static):
  * farjump_80xxxxxx entries in build/modules/stgNN.map (REL24 imports beyond +-32 MB; the module
    calls the far-jump stub which tail-jumps to the DOL address)
  * InvokeDirectCpu<0x80......u> and MKW_STATIC_TRANSLATED_CALL(0x80......) in
    generated/module_shards/stgNN/shard_*.cpp (and build/generated-stgNN/functions if present)
Native state: runtime/native-index.json registrations + a live scan of PPC_NATIVE_OVERRIDE* in
runtime/src (non-comment, enabled) vs SIN_DISABLED_NATIVE_OVERRIDE* (disabled).
Hardware-touch check: scans the DOL function body (main.dol + projects/r2ve01/MAP.txt) for
`lis rX,0xCC00/0xCC01` (write-gather pipe / hw registers), and transitively through bl callees.

Usage: python tools/module_imports.py
"""
import bisect
import json
import re
import struct
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODS = [f"stg0{i}" for i in range(8)]
DOL_LO, DOL_HI = 0x80000000, 0x80400000

# ---- names
sdk = {}
for l in (ROOT / "tools/sdk_name_map.txt").read_text().splitlines():
    p = l.split("\t")
    if len(p) >= 4 and not l.startswith("#"):
        sdk[int(p[0], 16)] = (p[1], p[2], p[3])
mapnames = {}
for l in (ROOT / "projects/r2ve01/MAP.txt").read_text().splitlines():
    m = re.match(r"^([0-9a-fA-F]{8})\s+(\S+)", l)
    if m:
        mapnames[int(m.group(1), 16)] = m.group(2)
starts = sorted(mapnames)

# ---- natives
idx = json.load(open(ROOT / "runtime/native-index.json"))["registrations"]
native_idx = defaultdict(list)
for r in idx:
    native_idx[int(r["address"], 16)].append((r["symbol"], r["sourceFile"], r.get("isTranslatedOverride")))
en_pat = re.compile(r"^\s*PPC_NATIVE_OVERRIDE(?:_VOID)?\(\s*([0-9A-Fa-f]{8})\s*,\s*(\w+)", re.M)
dis_pat = re.compile(r"^.*?SIN_DISABLED_NATIVE_OVERRIDE(?:_VOID)?\(\s*([0-9A-Fa-f]{8})\s*,\s*(\w+)", re.M)
native_src = defaultdict(list)
disabled_src = defaultdict(list)
for f in (ROOT / "runtime/src").rglob("*.cpp"):
    t = f.read_text(errors="replace")
    rel = f.relative_to(ROOT / "runtime/src").as_posix()
    for m in en_pat.finditer(t):
        native_src[int(m.group(1), 16)].append((m.group(2), rel))
    for m in dis_pat.finditer(t):
        disabled_src[int(m.group(1), 16)].append((m.group(2), rel))

# ---- DOL image
dol = (ROOT / "orig/R2VE01/sys/main.dol").read_bytes()
offs = struct.unpack_from(">18I", dol, 0)
addrs = struct.unpack_from(">18I", dol, 0x48)
lens = struct.unpack_from(">18I", dol, 0x90)
segs = [(addrs[k], lens[k], offs[k]) for k in range(18) if offs[k] and lens[k]]


def word(a):
    for base, ln, off in segs:
        if base <= a < base + ln:
            return struct.unpack_from(">I", dol, off + a - base)[0]
    return None


def fn_range(a):
    i = bisect.bisect_right(starts, a) - 1
    s = starts[i] if i >= 0 else a
    e = starts[i + 1] if i + 1 < len(starts) else s + 0x400
    return s, e


hw_cache = {}
_native_addrs = set(native_idx) | set(native_src)


def reach_hw(roots):
    """BFS over static bl edges from roots through non-native DOL functions.
    Returns {hw_fn: first_root} for functions with lis 0xCC00/0xCC01 that are not native."""
    found = {}
    seen = set()
    for r in roots:
        stack = [r]
        while stack:
            a = stack.pop()
            if a in seen or a in _native_addrs or not (DOL_LO <= a < DOL_HI):
                continue
            seen.add(a)
            s_, e_ = fn_range(a)
            for x in range(s_, e_, 4):
                w = word(x)
                if w is None:
                    break
                if (w >> 26) == 15 and ((w >> 16) & 31) == 0 and (w & 0xFFFF) in (0xCC00, 0xCC01):
                    found.setdefault(s_, r)
                if (w >> 26) == 18 and (w & 3) == 1:
                    li = (w >> 2) & 0xFFFFFF
                    if li & 0x800000:
                        li -= 0x1000000
                    stack.append((x + li * 4) & 0xFFFFFFFF)
    return found


def hw_touch(a, depth=0, seen=None):
    """Returns (direct_hw, transitive_hw_via) for a DOL function start."""
    if a in hw_cache and depth == 0:
        return hw_cache[a]
    s, e = fn_range(a)
    direct = False
    calls = []
    for x in range(s, e, 4):
        w = word(x)
        if w is None:
            break
        if (w >> 26) == 15 and ((w >> 16) & 31) == 0 and (w & 0xFFFF) in (0xCC00, 0xCC01):
            direct = True
        if (w >> 26) == 18 and (w & 3) == 1:
            li = (w >> 2) & 0xFFFFFF
            if li & 0x800000:
                li -= 0x1000000
            calls.append((x + li * 4) & 0xFFFFFFFF)
    via = None
    if not direct and depth < 2:
        seen = (seen or set()) | {a}
        for c in calls:
            if c in seen or c in _native_addrs or not (DOL_LO <= c < DOL_HI):
                continue
            d, v = hw_touch(c, depth + 1, seen)
            if d or v:
                via = c
                break
    r = (direct, via)
    if depth == 0:
        hw_cache[a] = r
    return r


# ---- callee collection
callees = defaultdict(lambda: defaultdict(int))  # addr -> module -> call sites (or 1 for stub)
for mod in MODS:
    stubs = {}
    for l in (ROOT / f"build/modules/{mod}.map").read_text().splitlines():
        m = re.match(r"^([0-9a-f]{8}) farjump_([0-9A-Fa-f]{8})$", l)
        if m:
            stubs[int(m.group(1), 16)] = int(m.group(2), 16)
    stubcount = defaultdict(int)
    dpat = re.compile(r"InvokeDirectCpu<0x([0-9A-Fa-f]{8})u>")
    spat = re.compile(r"MKW_STATIC_TRANSLATED_CALL\(\s*0x([0-9A-Fa-f]{8})")
    files = list((ROOT / "generated/module_shards" / mod).glob("shard_*.cpp"))
    for f in files:
        t = f.read_text(errors="replace")
        for m in dpat.finditer(t):
            a = int(m.group(1), 16)
            if a in stubs:
                stubcount[a] += 1
            elif DOL_LO <= a < DOL_HI:
                callees[a][mod] += 1
        for m in spat.finditer(t):
            a = int(m.group(1), 16)
            if DOL_LO <= a < DOL_HI:
                callees[a][mod] += 1
    for st, tgt in stubs.items():
        if DOL_LO <= tgt < DOL_HI:
            callees[tgt][mod] += max(stubcount.get(st, 0), 0)
            callees[tgt][mod + ":stub"] = 1


# ---- classification
def name_of(a):
    n = sdk.get(a)
    m = mapnames.get(a)
    return (n[0] if n else None), (n[1] if n else None), m


CLS = [("GX", r"^(__)?GX"), ("VI", r"^(__)?VI"), ("OS", r"^(__)?OS"), ("PAD", r"^(__)?(PAD|KPAD)"),
       ("WPAD/WUD/BTE", r"^(__)?(WPAD|WUD|BTE|BTA|btu|bta|WM)"), ("AX/AI/DSP", r"^(__)?(AX|AI|DSP)"),
       ("DVD", r"^(__)?DVD"), ("NAND/ISFS", r"^(__)?(NAND|ISFS|IOS|IPC)"), ("CARD/SC", r"^(__)?(CARD|SC|SYSCONF)"),
       ("EGG/nw4r", r"^(EGG|nw4r|__.*nw4r|.*__Q[234]4?nw4r|.*4nw4r)"), ("MATH/libc", r"^(PSMTX|PSVEC|MTX|VEC|C_MTX|sqrt|atan|sin|cos|mem|str|sprintf|printf|__)")]


def classify(n):
    if not n:
        return "game/unknown"
    if "nw4r" in n or re.search(r"__(Q\d|\d)", n) or n.startswith(("__ct__", "__dt__")):
        return "game/C++ (nw4r/EGG/Ng)"
    for k, p in CLS:
        if re.match(p, n):
            return k
    return "game/other"


GX_READONLY = re.compile(r"^GXGet|^GXReadXfRasMetric|^GXPeekZ|^GXPeekARGB|^GXGetGPStatus|^GXGetVertexDescList")
rows = []
for a in sorted(callees):
    mods = sorted(m for m in callees[a] if ":" not in m)
    n, lib, mp = name_of(a)
    nm = n or mp or f"fn_{a:08X}"
    cls = classify(n or mp)
    enabled_idx = native_idx.get(a, [])
    enabled_src = native_src.get(a, [])
    dis = disabled_src.get(a, [])
    direct, via = hw_touch(a)
    nat = bool(enabled_idx or enabled_src)
    must = False
    why = []
    if cls == "GX" and not GX_READONLY.match(nm):
        must = True
        why.append("GX state/FIFO writer")
    if direct:
        must = True
        why.append("lis 0xCC00/0xCC01 in body")
    elif via:
        must = True
        why.append(f"calls hw-touching fn_{via:08X}")
    if cls == "VI" or cls == "AX/AI/DSP" and False:
        pass
    rows.append(dict(addr=a, name=nm, lib=lib or "", cls=cls, mods=mods, sites=sum(v for k, v in callees[a].items() if ":" not in k),
                     native=nat, idx=enabled_idx, src=enabled_src, dis=dis, hw=direct, via=via, must=must, why=why))

# ---- write doc
out = ["# Module imports (DOL-resident callees of stg00..stg07)", "",
       "Generated by `python tools/module_imports.py`. Callees come from the module `.map` far-jump stubs "
       "(`farjump_80xxxxxx`), plus any `InvokeDirectCpu<0x80...u>` / `MKW_STATIC_TRANSLATED_CALL` in "
       "`generated/module_shards/stgNN`. Indirect calls (vtables, function pointers into the DOL) are not visible statically.",
       "", f"Total distinct DOL callees: **{len(rows)}**; natives present: **{sum(r['native'] for r in rows)}**; "
       f"MUST-native but missing: **{sum(1 for r in rows if r['must'] and not r['native'])}**.", ""]
out += ["## MUST be native and are NOT", "",
        "| Addr | Name | Lib | Class | Modules | Why | Disabled donor native here? |", "|---|---|---|---|---|---|---|"]
for r in rows:
    if r["must"] and not r["native"]:
        out.append(f"| {r['addr']:08X} | {r['name']} | {r['lib']} | {r['cls']} | {','.join(m[3:] for m in r['mods'])} | {'; '.join(r['why'])} | "
                   f"{', '.join(d[0] for d in r['dis']) or '-'} |")
trans = reach_hw([r["addr"] for r in rows])
out += ["", "## Hardware-register (0xCC00xxxx) functions reachable statically from module callees, with NO native", "",
        "Static `bl` closure through the DOL (stops at natives). These run translated bodies that write the unrouted gather pipe / hw regs.", "",
        "| HW fn | SDK/MAP name | Lib | Disabled donor native here | First module callee reaching it |", "|---|---|---|---|---|"]
for a in sorted(trans):
    n, lib, mp = name_of(a)
    out.append(f"| {a:08X} | {n or mp or ''} | {lib or ''} | {', '.join(d[0] for d in disabled_src.get(a, [])) or '-'} | {trans[a]:08X} {name_of(trans[a])[0] or name_of(trans[a])[2] or ''} |")
out += ["", "## Summary by class", "", "| Class | Callees | Native | Missing |", "|---|---|---|---|"]
bycls = defaultdict(list)
for r in rows:
    bycls[r["cls"]].append(r)
for k, v in sorted(bycls.items()):
    out.append(f"| {k} | {len(v)} | {sum(r['native'] for r in v)} | {sum(not r['native'] for r in v)} |")
out += ["", "## All callees", "",
        "| Addr | Name | Lib | Class | Mods | Sites | Native (enabled) | HW | Must |", "|---|---|---|---|---|---|---|---|---|"]
for k in sorted(bycls):
    for r in bycls[k]:
        nat = "; ".join(sorted({f"{s}({f})" for s, f in r["src"]} | {f"{s}({f})" for s, f, _ in r["idx"]})) or "-"
        hw = "direct" if r["hw"] else (f"via {r['via']:08X}" if r["via"] else "")
        out.append(f"| {r['addr']:08X} | {r['name']} | {r['lib']} | {r['cls']} | {','.join(m[3:] for m in r['mods'])} | {r['sites']} | {nat} | {hw} | {'MUST' if r['must'] else ''} |")
(ROOT / "docs/MODULE-IMPORTS.md").write_text("\n".join(out) + "\n")
json.dump(rows, open(ROOT / "build/module_imports.json", "w"), indent=1, default=str)
print(len(rows), "callees;", sum(1 for r in rows if r["must"] and not r["native"]), "must-native missing")
