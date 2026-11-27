#!/usr/bin/env python3
"""Generate a conservative PPC function-entry map for a Wii DOL.

The map is used by the translator both for function-boundary detection and as
speculative indirect-call seeds. Entries are evidence-based rather than a full
text scan: existing named symbols, frame-prologue instructions, constructor /
destructor table entries, and data pointers whose target looks like an entry
point (a frame prologue or the first instruction after a preceding blr).
"""

import argparse
import struct
from pathlib import Path


def u32be(data, offset):
    return struct.unpack_from(">I", data, offset)[0]


class Dol:
    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        offsets = struct.unpack_from(">18I", self.data, 0x00)
        addresses = struct.unpack_from(">18I", self.data, 0x48)
        sizes = struct.unpack_from(">18I", self.data, 0x90)
        self.sections = []
        for index in range(18):
            if sizes[index]:
                self.sections.append((addresses[index], addresses[index] + sizes[index], offsets[index], index))

    def word(self, address):
        for lo, hi, offset, index in self.sections:
            if lo <= address + 4 <= hi:
                return u32be(self.data, offset + address - lo)
        raise KeyError(hex(address))

    def executable(self, address):
        return any(index in (0, 1) and lo <= address < hi for lo, hi, _, index in self.sections)


def is_frame_prologue(word):
    # stwu r1, negative_immediate(r1); the standard PPC frame-opening instruction.
    return (word & 0xFFFF0000) == 0x94210000 and (word & 0x8000) != 0


def load_existing(path):
    entries = {}
    if not path or not Path(path).is_file():
        return entries
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        parts = line.strip().split(None, 1)
        if not parts:
            continue
        try:
            address = int(parts[0], 16)
        except ValueError:
            continue
        entries[address] = parts[1] if len(parts) > 1 else f"fn_{address:08X}"
    return entries


def generate(dol_path, existing_path=None):
    dol = Dol(dol_path)
    named = load_existing(existing_path)
    entries = dict(named)
    prologues = set()
    after_blr = set()
    data_pointers = set()
    ctor_entries = set()
    adjustor_thunks = set()

    executable_sections = [(lo, hi) for lo, hi, _, index in dol.sections if index in (0, 1)]
    text_lo = min((lo for lo, _ in executable_sections), default=0)

    def target_looks_like_entry(address):
        if not dol.executable(address):
            return False
        try:
            if is_frame_prologue(dol.word(address)):
                return True
        except KeyError:
            return False
        # A non-zero instruction immediately following a blr (allowing alignment
        # zeroes) is a strong entry-point shape. This also covers leaf functions
        # that do not open a frame.
        probe = address - 4
        try:
            while probe >= text_lo and dol.word(probe) == 0:
                probe -= 4
            # bctr (0x4E800420) also terminates the virtual-call tail-thunks
            # common in this binary. Data/vtable pointers immediately after
            # those thunks are real leaf entries even though they do not
            # follow a blr.
            terminator = dol.word(probe)
            if terminator in (0x4E800020, 0x4E800420):
                return True
            # Tail-call thunks also end in an unconditional branch (b, not bl).
            # When data explicitly points at the next instruction, that is a
            # real entry even when the method is too large for the small-leaf
            # shape. Jump-table interior labels do not normally sit immediately
            # after a tail-calling thunk, and interior entries are safe for the
            # translator's boundary repair even when the evidence is imperfect.
            return (terminator & 0xFC000003) == 0x48000000
        except KeyError:
            return False

    for lo, hi, _, index in dol.sections:
        if index in (0, 1):
            for address in range(lo, hi - 8, 4):
                try:
                    word = dol.word(address)
                    next_word = dol.word(address + 4)
                except KeyError:
                    continue
                if is_frame_prologue(word):
                    prologues.add(address)
                # C++ virtual-base adjusting thunks are commonly emitted as a
                # contiguous table of two-instruction leaf functions:
                #   addi r3,r3,-N ; b implementation
                # Their entries do not follow blr and have no frame prologue,
                # but they are frequent vtable targets and must be boundaries.
                if ((word & 0xFFFF0000) == 0x38630000 and
                        (next_word & 0xFC000003) == 0x48000000):
                    immediate = next_word & 0x03FFFFFC
                    if immediate & 0x02000000:
                        immediate -= 0x04000000
                    if dol.executable((address + 4 + immediate) & 0xFFFFFFFF):
                        adjustor_thunks.add(address)
        else:
            for address in range(lo, hi - 4, 4):
                try:
                    target = dol.word(address)
                except KeyError:
                    continue
                if target % 4 == 0 and dol.executable(target):
                    data_pointers.add(target)
                    # DOL sections 9/10 are _ctors/_dtors in the R2V layout. Every
                    # nonzero entry is by construction a function pointer, even
                    # when the target is a leaf with an unusual first instruction.
                    if index in (9, 10) and target != 0:
                        ctor_entries.add(target)

    for address in sorted(prologues):
        entries.setdefault(address, f"fn_{address:08X}")
    for address in sorted(adjustor_thunks):
        entries.setdefault(address, f"fn_{address:08X}")
    for address in sorted(ctor_entries):
        entries.setdefault(address, f"fn_{address:08X}")
    for address in sorted(data_pointers):
        if target_looks_like_entry(address):
            entries.setdefault(address, f"fn_{address:08X}")

    stats = {
        "existing": len(named),
        "prologues": len(prologues),
        "data_pointers": len(data_pointers),
        "ctor_entries": len(ctor_entries),
        "adjustor_thunks": len(adjustor_thunks),
        "total": len(entries),
    }
    return entries, stats


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dol")
    parser.add_argument("output")
    parser.add_argument("--existing")
    args = parser.parse_args()
    entries, stats = generate(args.dol, args.existing or args.output)
    Path(args.output).write_text(
        "".join(f"{address:08x} {name}\n" for address, name in sorted(entries.items())),
        encoding="utf-8",
    )
    print("generated {} entries (existing={}, prologues={}, data-pointers={}, ctors={}, adjustor-thunks={})".format(
        stats["total"], stats["existing"], stats["prologues"], stats["data_pointers"], stats["ctor_entries"],
        stats["adjustor_thunks"]))


if __name__ == "__main__":
    main()
