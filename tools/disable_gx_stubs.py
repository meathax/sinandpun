#!/usr/bin/env python3
"""Disable every GX fatal stub registration for Sin.

The donor's gx_fatal_stubs.cpp registers abort-by-design natives at MKWii
addresses whose functions the donor refused to implement. The retarget rewired
their literals onto Sin addresses by name, but Sin's layout puts UNRELATED
functions there (first hit: a donor stub keyed 801737f8 - which coincides with
Sin's BTE_Init address - excluded it from translation and aborted the game's
Bluetooth init during main()).

The translator's native-index builder regexes BOTH the registration macro and
the GX_FATAL_STUB(<addr>, ...) invocation itself (GeneratedMarkers.cs), so
every stub must be neutralized in BOTH spellings. Fatal stubs are donor
policy, not Sin policy: disable them all and let the guest's translated
functions run (Sin's GX natives already intercept the entry points that
matter).
"""
import re
import sys
from pathlib import Path

TARGET = Path(r"D:/Arcade/AI/aCORES/Sin/runtime/src/hle/gx/gx_fatal_stubs.cpp")
RE_OVERRIDE = re.compile(r"(\bPPC_NATIVE_OVERRIDE(_VOID)?\()([0-9A-Fa-f]{8})(\s*,)")
RE_STUB = re.compile(r"\bGX_FATAL_STUB\(")


def main():
    text = TARGET.read_text(encoding="utf-8")
    n_override = len(RE_OVERRIDE.findall(text))
    text = RE_OVERRIDE.sub(
        lambda m: f"{m.group(1).replace('PPC_NATIVE_OVERRIDE', 'SIN_DISABLED_NATIVE_OVERRIDE')}{m.group(3)}{m.group(4)}",
        text)
    n_stub = len(RE_STUB.findall(text))
    # Renaming the invocation breaks the runtime index builder's regex
    # (GeneratedMarkers.cs) so the stub address no longer excludes the
    # translated function; the defining macro keeps its name and is unused.
    text = RE_STUB.sub("GX_FATAL_STUB_DISABLED(", text)
    TARGET.write_text(text, encoding="utf-8")
    print(f"disabled {n_override} registrations and {n_stub} stub invocations")


if __name__ == "__main__":
    sys.exit(main())

