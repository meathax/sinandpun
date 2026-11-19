#!/usr/bin/env python3
"""Rewrite raw REGISTER_NATIVE_FUNCTION(0x..., NAME) calls to Sin addresses.

The macro pass (sin_retarget.py) only rewrites PPC_NATIVE_OVERRIDE* and
MKW_GADDR. Raw REGISTER_NATIVE_FUNCTION calls keep donor MKWii addresses,
which either hijack an unrelated Sin function or leave the real one
un-intercepted (boot crash: OSInit -> SIInit -> SISetSamplingRate ->
VIGetTvFormat MMIO, 2026-11-19).

Resolution: the registered C++ symbol encodes the guest function name
(OS__SendMessage_HLE_801a735c -> OSSendMessage); resolve it through the same
SDK evidence maps as the main bridge. Unresolved registrations are deleted
(not left at a wrong address); duplicates of a resolved macro at the same
target are dropped with a comment.
"""
import re
import sys
from pathlib import Path

ROOT = Path(r"D:/Arcade/AI/aCORES/Sin")
SRC = ROOT / "runtime/src"

sys.path.insert(0, str(ROOT / "tools"))
import importlib.util

_spec = importlib.util.spec_from_file_location("rc", ROOT / "tools/retarget_coverage2.py")
rc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rc)
exact, fallback = rc.lookup_maps()

RAW_RE = re.compile(r"REGISTER_NATIVE_FUNCTION\((0x[0-9A-Fa-f]{8})\s*,\s*(\w+)\)")


def guest_name(symbol: str) -> str:
    n = symbol
    n = re.sub(r"_HLE(_[0-9A-Fa-f]{8})?$", "", n)
    n = re.sub(r"_80[0-9A-Fa-f]{6}$", "", n)
    n = re.sub(r"^nw4r__", "nw4r::", n)
    n = n.replace("__", "::")
    n = re.sub(r"::+", "::", n)
    return n


def main():
    rewritten = deleted = 0
    for path in sorted(SRC.rglob("*.cpp")):
        text = orig = path.read_text(encoding="utf-8", errors="replace")

        def sub(m):
            nonlocal rewritten, deleted
            addr, symbol = m.group(1), m.group(2)
            gname = guest_name(symbol)
            hit = rc.resolve_name(gname, exact, fallback)
            if hit:
                rewritten += 1
                return f"REGISTER_NATIVE_FUNCTION(0x{hit[0].upper()}, {symbol})"
            deleted += 1
            return f"// SIN-PENDING(unresolved raw registration) {m.group(0)}"

        text = RAW_RE.sub(sub, text)
        if text != orig:
            path.write_text(text, encoding="utf-8")
            print(f"updated {path.relative_to(ROOT)}")
    print(f"raw registrations rewritten: {rewritten}, deleted: {deleted}")


if __name__ == "__main__":
    sys.exit(main())
