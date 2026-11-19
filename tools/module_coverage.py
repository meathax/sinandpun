#!/usr/bin/env python3
"""List every statically visible code address of a stage module that has no translated func_*.cpp.

Targets considered (all must be a function entry for the runtime dispatcher to find them):
  bl / data pointers / lis+addi (or ori) constants / prologue-after-blr / out-of-span b,bc tail targets
Targets inside the module text are checked against build/generated-stgNN/functions. Targets in the DOL
range (0x80xxxxxx) are checked against that dir plus generated/functions (the base DOL translation).
Jump-table case labels are reported separately (they live inside their function by design, but any that is
ALSO called/pointed-to elsewhere shows up in the main list).

Usage: python tools/module_coverage.py [stg00 ...]   (default: all 8)   [--list] [--out FILE]
"""
import argparse
import bisect
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import module_seeds as ms  # noqa: E402

ROOT = ms.ROOT


def funcs_in(d):
    out = set()
    if d.is_dir():
        for f in d.iterdir():
            n = f.name
            if n.startswith("func_") and n.endswith(".cpp"):
                try:
                    out.add(int(n[5:13], 16))
                except ValueError:
                    pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*")
    ap.add_argument("--list", action="store_true", help="print every uncovered address")
    ap.add_argument("--out", help="write full report here")
    ap.add_argument("--gen", help="generated dir template, {name} substituted (default build/generated-{name})")
    a = ap.parse_args()
    names = a.names or [f"stg0{i}" for i in range(8)]
    dol = funcs_in(ROOT / "generated/functions")
    rows = []
    report = []
    for name in names:
        m = ms.Module(name)
        gen = funcs_in(ROOT / (a.gen or "build/generated-{name}").format(name=name) / "functions")
        if not gen:
            print(f"{name}: no translated functions found")
            continue
        have = gen | dol
        mod_entries = sorted(x for x in gen if m.base <= x < m.base + len(m.img) + 0x10000)
        cands, jt_labels, tables = ms.scan(m)
        import json
        mp = ROOT / (a.gen or "build/generated-{name}").format(name=name) / "base_translation_output.json"
        local_labels = set()
        for fn in json.load(open(mp))["functions"]:
            local_labels.update(fn.get("localLabelAddresses", []))
        unc = {}

        def note(addr, src):
            unc.setdefault(addr, set()).add(src)

        for addr, srcs in cands.items():
            if addr in have:
                continue
            for s in srcs:
                note(addr, s)
        # branch targets outside the module text (DOL calls, far-jump stubs)
        import struct
        for i, w in enumerate(m.words):
            pc = m.tbase + i * 4
            bt = ms.branch_target(w, pc)
            if not bt:
                continue
            tgt, link, kind = bt
            if m.in_text(tgt):
                if not link and tgt not in have and tgt not in local_labels:
                    # not an entry and not an internal label of any translated function
                    k = bisect.bisect_right(mod_entries, pc) - 1
                    lo = mod_entries[k] if k >= 0 else m.tbase
                    hi = mod_entries[k + 1] if k + 1 < len(mod_entries) else m.tend
                    if not (lo <= tgt < hi):
                        note(tgt, "tail")
            elif tgt % 4 == 0 and tgt not in have:
                note(tgt, "bl-ext" if link else "tail-ext")
        for a_ in m.map_text:
            if a_ not in gen:
                note(a_, "map-skipped")
        for a_ in jt_labels:
            if a_ not in gen and a_ not in local_labels:
                note(a_, "jt-unrecognised")
        # classify
        cat = {}
        for addr, srcs in unc.items():
            if addr in jt_labels and "jt-unrecognised" not in srcs and not (srcs & {"bl", "prologue", "bl-ext", "data", "lisaddi"}):
                kind = "jt-case"
            elif m.in_text(addr):
                k = bisect.bisect_right(mod_entries, addr) - 1
                interior = k >= 0 and mod_entries[k] != addr and addr < (mod_entries[k + 1] if k + 1 < len(mod_entries) else m.tend)
                kind = "interior" if interior else "no-owner"
            else:
                kind = "external"
            cat.setdefault(kind, []).append(addr)
        n_real = sum(len(v) for k, v in cat.items() if k != "jt-case")
        line = f"{name}: candidates={len(cands)} translated(mod)={len(mod_entries)} UNCOVERED={n_real} " + " ".join(
            f"{k}={len(v)}" for k, v in sorted(cat.items()))
        print(line)
        report.append(line)
        for k, v in sorted(cat.items()):
            if k == "jt-case":
                continue
            for addr in sorted(v):
                cl = ms.classify(m, addr, unc[addr], jt_labels) if m.in_text(addr) else "-"
                fw = m.w(addr) if m.in_text(addr) else 0
                row = f"  {name} {addr:08X} {k:9} {cl:14} src={','.join(sorted(unc[addr]))} first={fw:08X}"
                report.append(row)
                if a.list:
                    print(row)
        rows.append((name, n_real))
    if a.out:
        Path(a.out).write_text("\n".join(report) + "\n")
    print("TOTAL uncovered:", sum(n for _, n in rows))
    return 1 if any(n for _, n in rows) else 0


if __name__ == "__main__":
    sys.exit(main())
