#!/usr/bin/env python3
"""Retarget the runtime sources from MKWii PAL identities to R2VE01 addresses.

Rules:
- PPC_NATIVE_OVERRIDE*(<addr>, ...) with a resolved donor address -> rewrite the
  literal to the Sin address.
- Unresolved registrations -> rename the macro to SIN_DISABLED_NATIVE_OVERRIDE
  (_VOID), which hle_stubs.h defines as an empty variadic macro. The guest's own
  translated function then runs instead of a wrong-address hijack.
- MKW_GADDR(_CT)?(<addr>) resolved -> rewrite; unresolved -> left in place and
  reported to tools/retarget_pending_gaddr.txt for follow-up.

Run from the repository root. Idempotent: donor->sin is a total function, so a
second run rewrites sin->sin to the same value... except Sin addresses that equal
a donor address would re-resolve; the pending report is the source of truth.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(r"D:/Arcade/AI/aCORES/Sin")
SRC = ROOT / "runtime/src"
RESOLVED = ROOT / "tools/retarget_resolved.json"

# Registrations in these files are disabled for the first Sin boot round even
# when resolved: their surrounding code depends on MKWii globals/GADDR identities
# that have no verified R2VE01 counterpart yet (fiber scheduler state, EGG
# display plumbing, MKWii product glue). Re-enable per file once its constants
# are recovered; tools/retarget_pending_gaddr.txt names the gaps.
BLOCKLIST_FILES = {
    "src/fiber_manager.cpp",
    "src/hle/os/os_scheduler.cpp",
    "src/hle/os/os_thread.cpp",
    "src/hle/task_thread.cpp",
    "src/hle/boot_race.cpp",
    "src/hle/strap_scene.cpp",
    "src/friend_code_keyboard.cpp",
    "src/hle/egg_decomp.cpp",
    "src/hle/gx/gx_egg.cpp",
    "src/dynamic_aspect.cpp",
    "src/music_attenuation.cpp",
    "src/recomp_mod_loader.cpp",
    "src/hle/gx/gx_dynamic_aspect.h",
    # Abort-by-design donor stubs: their retargeted literals land on unrelated
    # Sin functions (first casualty: BTE_Init 0x801737F8 excluded from
    # translation by GX_FATAL_STUB). Donor stub policy does not transfer.
    "src/hle/gx/gx_fatal_stubs.cpp",
}

OVERRIDE_RE = re.compile(
    r"PPC_NATIVE_OVERRIDE(_VOID)?\(([0-9A-Fa-f]{8})\s*,")
GADDR_RE = re.compile(
    r"MKW_GADDR(_CT)?\(([0-9A-Fa-f]{8})\)")


def main():
    data = json.loads(RESOLVED.read_text())
    donor2sin = {}
    for label in ("registrations", "gaddr"):
        for row in data.get(label, []):
            daddr, name, saddr = row.split("\t")
            donor2sin[daddr.upper()] = saddr.upper()

    print(f"resolved identities: {len(donor2sin)}")
    reg_ok = reg_off = 0
    ga_ok = 0
    ga_pending = set()
    pending_lines = []
    changed_files = 0
    for path in sorted(list(SRC.rglob("*.cpp")) + list(SRC.rglob("*.h"))):
        rel = path.relative_to(ROOT / "runtime")
        text = orig = path.read_text(encoding="utf-8", errors="replace")
        stats = [0, 0, 0, 0]

        def sub_override(m):
            is_void, addr = m.group(1) or "", m.group(2).upper()
            stats[2] += 1
            if str(rel).replace("\\", "/") in BLOCKLIST_FILES:
                stats[1] += 1
                return f"SIN_DISABLED_NATIVE_OVERRIDE{is_void}({m.group(2)},"
            if addr in donor2sin:
                stats[0] += 1
                return f"PPC_NATIVE_OVERRIDE{is_void}({donor2sin[addr]},"
            stats[1] += 1
            return f"SIN_DISABLED_NATIVE_OVERRIDE{is_void}({m.group(2)},"

        def sub_gaddr(m):
            ct, addr = m.group(1) or "", m.group(2).upper()
            if addr in donor2sin:
                stats[3] += 1
                return f"MKW_GADDR{ct}({donor2sin[addr]})"
            pending_lines.append(f"{rel}\t{m.group(2)}")
            return m.group(0)

        text = OVERRIDE_RE.sub(sub_override, text)
        text = GADDR_RE.sub(sub_gaddr, text)
        if text != orig:
            path.write_text(text, encoding="utf-8")
            changed_files += 1
        reg_ok += stats[0]
        reg_off += stats[1]
        ga_ok += stats[3]

    print(f"registrations rewritten: {reg_ok}, disabled: {reg_off}")
    print(f"MKW_GADDR rewritten: {ga_ok}, pending: {len(pending_lines)}")
    print(f"files changed: {changed_files}")
    (ROOT / "tools/retarget_pending_gaddr.txt").write_text(
        "\n".join(sorted(set(pending_lines))), encoding="utf-8")
    print("wrote tools/retarget_pending_gaddr.txt")


if __name__ == "__main__":
    sys.exit(main())
