#!/usr/bin/env python3
"""Audio HLE retarget report (AX / AI / DSP / MIX / nw4r snd / HBM sound) for Sin & Punishment.

Analogue of tools/gx_retarget_report.py for the audio surface. Static analysis only (no build, no run):

  1. SDK AUDIO FUNCTIONS: every tools/sdk_name_map.txt entry in an audio library (ax, axart, axfx, ai,
     dsp, mix, libnw4r_snd, plus audio-named homebuttonLib/HBM entries), with
       NATIVE   = a PPC_NATIVE_OVERRIDE / REGISTER_NATIVE_FUNCTION is registered at the Sin address
                  (runtime/native-index.json)
       DISABLED = a SIN_DISABLED_* donor native with the same derived SDK name exists (not live)
       real     = the translated guest code runs
     and CALLED (first-call evidence from a SIN_CALLTRACE run, build/native/calltrace.txt).
  2. HARDWARE TOUCHERS: translated functions (generated/functions/func_*.cpp) that access DSP / ARAM-DMA /
     AI-DMA / AI / hollywood-clock MMIO (lis 0xCC00/0xCD00/0xCD80 + the audio offsets). Those silently do
     nothing (or hang on a status spin) unless a native covers them.
  3. INLINED NATIVES: functions that contain "inline leaf 0xADDR" for an ADDR that has a native: the
     native is bypassed there (the leaf's real code runs), so it must be state-compatible.
  4. DONOR ADDRESS AUDIT of runtime/src/hle/audio/*: every MKW_GADDR/MKW_GUEST_FUNC/raw 0x80xxxxxx constant,
     classified against the Sin function set and tools/sdk_globals_map.txt; unverified ones are flagged.

Usage: python tools/audio_retarget_report.py [--all] [--no-hw] [--calltrace FILE]
"""
import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ap = argparse.ArgumentParser()
ap.add_argument("--all", action="store_true", help="also list audio SDK functions that are not called")
ap.add_argument("--no-hw", action="store_true", help="skip the (slow, ~10 s) MMIO scan")
ap.add_argument("--calltrace", default=None, help="default: newest of build/native/calltrace.txt, build/calltrace_*.txt")
args = ap.parse_args()

AUDIO_LIBS = {"ax.a", "axart.a", "axfx.a", "ai.a", "dsp.a", "mix.a", "libnw4r_snd.a"}
AUDIO_NAME = re.compile(r"HBM(Ax|MIX|SYN|SEQ)|AxSound|RemoteSpk|Speaker|__AX|AXFX|^AX|^AI|^DSP|^MIX|nw4r3snd")

names = {}  # addr -> [(name, lib, quality)]
for line in (ROOT / "tools/sdk_name_map.txt").read_text().splitlines():
    p = line.split("\t")
    if len(p) >= 4 and p[3] in ("start", "interior"):
        names.setdefault(int(p[0], 16), []).append((p[1], p[2], p[3]))

native_idx = json.load(open(ROOT / "runtime/native-index.json"))["registrations"]
native = {int(r["address"], 16): r for r in native_idx}

disabled = []
pat = re.compile(r"SIN_DISABLED_NATIVE_OVERRIDE(?:_VOID)?\(\s*([0-9A-Fa-f]{8})\s*,\s*(\w+)")
for f in (ROOT / "runtime/src").rglob("*.cpp"):
    for m in pat.finditer(f.read_text(errors="replace")):
        disabled.append((f.relative_to(ROOT / "runtime/src").as_posix(), m.group(1).upper(), m.group(2)))


def derive(sym):
    s = re.sub(r"_[0-9a-fA-F]{8}$", "", sym)
    s = re.sub(r"_(HLE|hle)$", "", s)
    s = re.sub(r"_[0-9a-fA-F]{8}$", "", s)
    return re.sub(r"^(OS|VI|AI|AX|DVD|NAND|IPC|SC|WPAD|KPAD|PAD|ARC)__", r"\1", s)


disabled_by_name = {}
for src, addr, sym in disabled:
    disabled_by_name.setdefault(derive(sym), []).append((src, addr, sym))

seen = set()
cands = [Path(args.calltrace)] if args.calltrace else sorted(
    [p for p in [ROOT / "build/native/calltrace.txt", *(ROOT / "build").glob("calltrace_*.txt")] if p.exists() and p.stat().st_size],
    key=lambda p: p.stat().st_mtime, reverse=True)
ct = cands[0] if cands else Path("/nonexistent")
if ct.exists():
    seen = {int(l.split()[1], 16) for l in ct.read_text().splitlines() if len(l.split()) >= 2}

func_files = {int(p.stem[5:13], 16) for p in (ROOT / "generated/functions").glob("func_*.cpp")}

rows = []
for a, lst in sorted(names.items()):
    for n, lib, q in lst:
        if lib in AUDIO_LIBS or (lib == "homebuttonLib.a" and AUDIO_NAME.search(n)):
            rows.append((a, n, lib))

print(f"== 1. SDK audio functions ({len(rows)}); calltrace {ct if seen else 'MISSING'} ({len(seen)} targets)")
print("   state: NATIVE=enabled at Sin addr, DISABLED=donor native exists but is off, real=guest code runs")
for a, n, lib in rows:
    state = "NATIVE" if a in native else ("DISABLED" if derive(n) in disabled_by_name else "real")
    called = a in seen
    if not (called or args.all or state != "real"):
        continue
    extra = ""
    if a in native:
        extra = f"  <- {native[a]['sourceFile']} {native[a]['symbol']}"
    elif derive(n) in disabled_by_name:
        extra = "  <- " + ", ".join(f"{s} {sym} (donor {ad})" for s, ad, sym in disabled_by_name[derive(n)])
    print(f"  {a:08X} {state:8s} {'CALLED' if called else '      '} {lib:14s} {n[:70]}{extra}")

HW_RANGES = [
    ("DSP mailbox/ctrl", 0xCC005000, 0xCC00501F),
    ("ARAM DMA", 0xCC005020, 0xCC00502F),
    ("AI DMA", 0xCC005030, 0xCC00503F),
    ("AI regs", 0xCC006C00, 0xCC006C3F),
    ("AI regs(CD)", 0xCD006C00, 0xCD006C3F),
    ("hollywood clk", 0xCD800180, 0xCD8001FF),
]
callers_inline = {}
hw_hits = {}
if not args.no_hw:
    lit = re.compile(r"-?\d{8,10}")
    off = re.compile(r"\(r\d+ \+ (-?\d+)\)")
    inl = re.compile(r"inline leaf 0x([0-9A-Fa-f]{8})")
    for f in (ROOT / "generated/functions").glob("func_*.cpp"):
        t = f.read_text(errors="replace")
        a = int(f.stem[5:13], 16)
        for m in inl.finditer(t):
            callers_inline.setdefault(int(m.group(1), 16), set()).add(a)
        bases = {int(v) & 0xFFFFFFFF for v in lit.findall(t)} & {0xCC000000, 0xCD000000, 0xCD800000}
        if not bases:
            continue
        offs = {int(x) & 0xFFFFFFFF for x in off.findall(t)}
        for b in bases:
            for o in offs:
                addr = b + o
                for name, lo, hi in HW_RANGES:
                    if lo <= addr <= hi:
                        hw_hits.setdefault(a, set()).add(name)
    print()
    print(f"== 2. Hardware touchers (DSP/ARAM/AI/clock MMIO): {len(hw_hits)} functions")
    for a in sorted(hw_hits):
        nm = ",".join(n for n, _, _ in names.get(a, [])[:1]) or "?"
        state = "NATIVE" if a in native else "NO NATIVE"
        print(f"  {a:08X} {state:9s} {'CALLED' if a in seen else '      '} {nm[:34]:34s} {sorted(hw_hits[a])}")

    print()
    print("== 3. Natives bypassed by leaf inlining (real leaf runs inside the caller)")
    any3 = False
    for a in sorted(callers_inline):
        if a in native:
            any3 = True
            print(f"  native {a:08X} {native[a]['symbol']} inlined into: " +
                  ", ".join(f"{c:08X}" for c in sorted(callers_inline[a])))
    if not any3:
        print("  none")
    print("  audio leaves inlined without a native (check they only read/write plain guest memory):")
    for a in sorted(callers_inline):
        if a not in native and a in names and any(l in AUDIO_LIBS for _, l, _ in names[a]):
            print(f"  {a:08X} {names[a][0][0]} inlined into: " + ", ".join(f"{c:08X}" for c in sorted(callers_inline[a])))

# Constants verified by reading translated bodies (see docs/AUDIO-NOTES.md section 4).
VERIFIED = {
    0x803D89A0: "__AXDSPTask (lis 0x803E,-30304 in __AXOutInitDSP 8015C170)",
    0x803D8A00: "__AXDramImage (lis 0x803E,-30208 in 8015C170)",
    0x8015C0E0: "__AXDSPInitCallback (task+0x28, sets __AXDSPInitFlag)",
    0x8015C150: "__AXDSPDoneCallback (task+0x30, sets __AXDSPDoneFlag)",
    0x8015C160: "__AXDSPRequestCallback (task+0x34)",
    0x80159550: "AIRegisterDMACallback (r13-22720 swap)",
}
gmap = {}
for line in (ROOT / "tools/sdk_globals_map.txt").read_text().splitlines():
    p = line.split("\t")
    if len(p) == 2 and re.fullmatch(r"[0-9A-Fa-f]{8}", p[0]):
        gmap[int(p[0], 16)] = p[1]

print()
print("== 4. Address constants in runtime/src/hle/audio (and music_attenuation.cpp)")
print("   FUNC? = Sin function without an SDK name; DONOR? = no Sin function there and not a known Sin")
print("   global in tools/sdk_globals_map.txt (probably an MKWii address, or an unnamed Sin data address:")
print("   verify against a translated body before trusting it)")
adr = re.compile(r"(?:MKW_GADDR|MKW_GUEST_FUNC|REGISTER_NATIVE_FUNCTION(?:_AS)?|PPC_NATIVE_OVERRIDE(?:_VOID)?)\(\s*(?:0x)?([0-9A-Fa-f]{8})")
files = list((ROOT / "runtime/src/hle/audio").glob("*")) + [ROOT / "runtime/src/music_attenuation.cpp"]
for f in sorted(files):
    if not f.is_file():
        continue
    seen_here = set()
    for i, l in enumerate(f.read_text(errors="replace").splitlines(), 1):
        code = l.split("//")[0]
        for m in adr.finditer(code):
            a = int(m.group(1), 16)
            if a in seen_here:
                continue
            seen_here.add(a)
            if a in VERIFIED:
                st, nm = "OK", VERIFIED[a]
            elif a in func_files:
                st = "OK" if a in names else "FUNC?"
                nm = names[a][0][0][:40] if a in names else ""
            elif a in gmap:
                st, nm = "OK", gmap[a][:40]
            elif a in names:
                st, nm = "OK", names[a][0][0][:40]
            else:
                st, nm = "DONOR?", ""
            if st != "OK":
                print(f"  {st:7s} {f.name}:{i}: {a:08X} {nm}")
print()
print("Sin audio globals known from translated AIInit/DSPInit/__AXOutInitDSP (r13=0x804B7700):")
for a, n in sorted(gmap.items()):
    if re.match(r"__(AI|AXAi|AXDSP|AXOut|DSP_)|axDsp|__AXVPB|__AXPB|__AXMax|__MIX", n):
        print(f"  {a:08X} {n}")
