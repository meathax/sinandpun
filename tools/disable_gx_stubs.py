#!/usr/bin/env python3
"""Disable every GX fatal stub registration for Sin.

The donor's gx_fatal_stubs.cpp registers abort-by-design natives at MKWii
addresses whose functions the donor refused to implement. The retarget rewired
their literals onto Sin addresses by name, but Sin's layout puts UNRELATED
functions there (first hit: GX_FATAL_STUB landed on BTE_Init 0x801737F8,
excluding it from translation and aborting the game's Bluetooth init during
main()). Fatal stubs are donor policy, not Sin policy: disable them all and
let the guest's translated functions run (Sin's GX natives already intercept
the entry points that matter).
"""
import re
import sys
from pathlib import Path

TARGET = Path(r"D:/Arcade/AI/aCORES/Sin/runtime/src/hle/gx/gx_fatal_stubs.cpp")
RE = re.compile(r"(\bPPC_NATIVE_OVERRIDE(_VOID)?\()([0-9A-Fa-f]{8})(\s*,)")


def main():
    text = TARGET.read_text(encoding="utf-8")
    hits = RE.findall(text)
    text2 = RE.sub(lambda m: f"{m.group(1).replace('PPC_NATIVE_OVERRIDE', 'SIN_DISABLED_NATIVE_OVERRIDE')}{m.group(3)}{m.group(4)}", text)
    TARGET.write_text(text2, encoding="utf-8")
    print(f"disabled {len(hits)} fatal-stub registrations in {TARGET.name}")
    for m in RE.finditer(text):
        pass


if __name__ == "__main__":
    sys.exit(main())
