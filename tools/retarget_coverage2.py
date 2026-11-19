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


def lookup_maps():
    """Build (exact, fallback) SDK lookups.

    exact:    lowercase full name -> entry. Trusted.
    fallback: normalized name -> entry, but ONLY when that normalized key
    collapses to one distinct source name; generic suffixes (AIInit/DSPInit/
    GXInit all -> 'init') make a key ambiguous and it is dropped.
    """
    raw = []  # (addr, name, source)
    for path, columns in ((ROOT / "tools/sdk_name_map.txt", 3),
                          (ROOT / "tools/sdk_data_map.txt", 3),
                          (ROOT / "tools/sdk_globals_map.txt", 2),
                          (ROOT / "tools/sdk_layout_map.txt", 2)):
        for line in path.read_text().splitlines():
            if line.startswith("#") or not line.strip():
                continue
            parts = line.split("\t")
            raw.append((parts[0], parts[1], path.name))
    exact = {}
    for addr, name, src in raw:
        exact.setdefault(name.lower(), (addr, name, src))
    by_norm = {}
    for addr, name, src in raw:
        by_norm.setdefault(norm(name), set()).add(name.lower())
    fallback = {}
    for key, sources in by_norm.items():
        if len(sources) == 1:
            src = sources.pop()
            for addr, name, s in raw:
                if name.lower() == src:
                    fallback[key] = (addr, name, s)
                    break
    return exact, fallback


def resolve_name(name, exact, fallback):
    dm = cw_demangle(name) or name
    # Rule 1: exact (also handles donor "__Lib::Fn" -> SDK "__LibFn").
    candidates = [dm, name]
    m = re.match(r"^__(\w+)::(\w+)$", dm)
    if m:
        candidates.insert(0, f"__{m.group(1)}{m.group(2)}")
    for c in candidates:
        hit = exact.get(c.lower())
        if hit:
            return hit
    # Rule 2: normalized fallback (ambiguity-filtered).
    return fallback.get(norm(name))


def main():
    exact, fallback = lookup_maps()

    donor = {}
    for line in (ROOT / "reference/Wiicompiled/projects/mkwii/MAP.txt").read_text(
            encoding="utf-8", errors="replace").splitlines():
        m = MAP_LINE.match(line.strip())
        if m:
            donor.setdefault(int(m.group(1), 16), m.group(2))

    # Donor constants the MKWii map does not name, aliased to SDK symbols by
    # source-level semantic (verified per constant, see tools/alias_gaddr.json).
    alias = json.loads((ROOT / "tools/alias_gaddr.json").read_text())
    for a, sdkname in alias.items():
        if a.startswith("_"):
            continue
        donor.setdefault(int(a, 16), str(sdkname))

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
            hit = resolve_name(name, exact, fallback)
            if hit:
                ok.append((a, name, hit[0]))
            else:
                miss.append((a, name))
        out[label] = (ok, miss, unnamed)

    # Collisions: two donor identities resolving to the same Sin address prove
    # at least one resolution wrong. Disable BOTH (drop from the resolved set)
    # so the retarget never wires a native to an ambiguous target.
    sizes = {"registrations": len(regs), "gaddr": len(gaddr)}
    for label in ("registrations", "gaddr"):
        ok, miss, unnamed = out[label]
        by_sin = {}
        for a, name, sin in ok:
            by_sin.setdefault(sin.upper(), []).append((a, name))
        dropped = {sin for sin, lst in by_sin.items() if len(lst) > 1}
        if dropped:
            print(f"{label}: dropping {len(dropped)} colliding Sin addresses:")
            for sin in sorted(dropped):
                for a, name in by_sin[sin]:
                    print(f"    {a:08X} {name} -> {sin}")
        out[label] = ([row for row in ok if row[2].upper() not in dropped], miss, unnamed)
        print(f"{label}: {sizes[label]} needed, {len(out[label][0])} resolved, "
              f"{len(miss)} unmatched, {len(unnamed)} unnamed-in-donor")
        for a, n in miss[:15]:
            print(f"    miss {a:08X} {n}")

    (ROOT / "tools/retarget_resolved.json").write_text(json.dumps({
        label: [f"{a:08X}\t{n}\t{sin}" for a, n, sin in v[0]] for label, v in out.items()}, indent=1))
    print("\nwrote tools/retarget_resolved.json")
    print(f"SDK names: {len(exact)} exact, {len(fallback)} unambiguous-normalized")


if __name__ == "__main__":
    sys.exit(main())
