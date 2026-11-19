#!/usr/bin/env python3
"""Probe: where does one SDK function best-match inside the Sin DOL .text?

Extracts a named function from an SDK .a (masked, reloc-aware), then slides
over the game's .text at word alignment and reports the best candidates by
masked-word difference count. Verifies whether the Feb-2009 (game) vs
Aug-2010 (archive) build delta is small enough for fuzzy matching.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import importlib.util

ROOT = Path(r"D:/Arcade/AI/aCORES/Sin")
sys.path.insert(0, str(ROOT / "tools"))
from sdk_match import read_dol, extract_functions, words_masked  # noqa: E402


def probe(lib_name, func_name, max_report=5):
    lib = ROOT / "build/sdk/x/wii_development_package/RVL_SDK/RVL/lib" / lib_name
    target = None
    for fname, tbytes, masked in extract_functions(lib):
        if fname == func_name:
            target = (fname, tbytes, masked)
            break
    if target is None:
        print(f"{func_name}: not found in {lib_name}")
        return
    fname, tbytes, masked = target
    mw = words_masked(tbytes, masked)
    n = len(mw)
    print(f"{fname}: {len(tbytes)} bytes, {n} words, {len(masked)} masked")

    sections = read_dol(ROOT / "orig/R2VE01/sys/main.dol")
    _ta, tl, text = next((ea, ln, b) for ea, ln, b in sections if ea == 0x8001F240)
    total_words = tl // 4

    tw = struct.unpack_from(f">{total_words}I", text, 0)

    # Slide: at each position, count diffs over non-masked words.
    best = []
    for pos in range(0, total_words - n):
        diffs = 0
        for i, w in enumerate(mw):
            if w is None:
                continue
            if tw[pos + i] != w:
                diffs += 1
                if diffs > 8:
                    break
        if diffs <= 8:
            best.append((diffs, 0x8001F240 + pos * 4))
    best.sort()
    shown = []
    for d, a in best[:40]:
        if shown and a - shown[-1][1] < 32:
            continue
        shown.append((d, a))
        if len(shown) >= max_report:
            break
    print(f"candidates (diffs, addr): {shown[:max_report]}")
    if not shown:
        print("no candidate within 8 diffs")


if __name__ == "__main__":
    probe(sys.argv[1] if len(sys.argv) > 1 else "os.a",
          sys.argv[2] if len(sys.argv) > 2 else "OSSendMessage")
