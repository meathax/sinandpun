#!/usr/bin/env python3
"""Check the R2VE01 -> R2VJ01 guest-address table against the projects and DOLs.

1. Every translator address list in projects/r2vj01/recomp.yml (entry points, loop-poll
   functions, entry hooks) must equal the R2VE01 list mapped through
   runtime/include/guest_abi/guest_versions.inc. A missing Japanese entry hook silently
   disables the runtime hook that GuestEntryHook compares against MKW_GADDR(...).
2. With both main.dol files available, every mapped .text row whose USA address is a
   function start must map to a Japanese function start, and no nearby Japanese function
   may match the USA body clearly better. This catches rows that land a few words inside a
   function (the native never fires) or on a neighbouring function (the wrong native fires).
3. Every small-data (r13/r2) row must be the variable the matching Japanese function
   accesses in the position where the USA function accesses the USA variable.

Usage: python tools/check_region_map.py [--usa-dol PATH] [--jpn-dol PATH]
"""

import argparse
import re
import struct
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLE = ROOT / "runtime/include/guest_abi/guest_versions.inc"
ROW = re.compile(r"MKW_REGION_RANGE\(NtscJ,\s*([0-9A-F]+),\s*([0-9A-F]+),\s*([+-]0x[0-9A-F]+)\)")
LISTS = ("entry_points", "loop_poll_functions", "entry_hooks")
BLR = 0x4E800020


def load_table(path=TABLE):
    rows = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        m = ROW.match(line.strip())
        if m:
            rows.append((int(m[1], 16), int(m[2], 16), int(m[3], 16)))
    return rows


def map_address(rows, address):
    for first, last, delta in rows:
        if first <= address <= last:
            return address + delta
    return None


def project_lists(path):
    """Address lists under translation: in a recomp.yml (simple block-list parser)."""
    lists, current = {}, None
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        key = stripped.rstrip(":")
        if stripped.endswith(":") and key in LISTS:
            current = key
            lists[current] = []
            continue
        if current and stripped.startswith("- "):
            value = stripped[2:].split("#", 1)[0].strip()
            lists[current].append(int(value, 16))
            continue
        if current and stripped and not stripped.startswith("#"):
            current = None
    return lists


def check_projects(rows, usa_yml, jpn_yml):
    errors = []
    usa, jpn = project_lists(usa_yml), project_lists(jpn_yml)
    for name in LISTS:
        expected = set()
        for address in usa.get(name, []):
            mapped = map_address(rows, address)
            if mapped is None:
                errors.append(f"{name}: R2VE01 0x{address:08X} has no R2VJ01 row in guest_versions.inc")
            else:
                expected.add(mapped)
        actual = set(jpn.get(name, []))
        for address in sorted(expected - actual):
            errors.append(f"{name}: projects/r2vj01/recomp.yml lacks 0x{address:08X}")
        for address in sorted(actual - expected):
            errors.append(f"{name}: projects/r2vj01/recomp.yml has unmapped extra 0x{address:08X}")
    return errors


class Dol:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        offsets = struct.unpack_from(">18I", self.data, 0x00)
        addresses = struct.unpack_from(">18I", self.data, 0x48)
        sizes = struct.unpack_from(">18I", self.data, 0x90)
        self.sections = [(addresses[i], addresses[i] + sizes[i], offsets[i], i) for i in range(18) if sizes[i]]

    def word(self, address):
        for lo, hi, offset, _ in self.sections:
            if lo <= address < hi:
                return struct.unpack_from(">I", self.data, offset + address - lo)[0]
        return None

    def is_text(self, address):
        return any(index < 7 and lo <= address < hi for lo, hi, _, index in self.sections)


def is_start(dol, address):
    previous = dol.word(address - 4)
    if previous is None:
        return False
    return previous in (BLR, 0, 0x60000000) or ((previous >> 26) == 18 and (previous & 1) == 0)


def normalize(word):
    op, rd, ra = word >> 26, (word >> 21) & 31, (word >> 16) & 31
    if op in (16, 18):
        return ("branch", op, word & 1)
    if op == 14 and ra == 0:
        return ("li", word & 0xFFFF)
    if op in (14, 15):
        return (op, "sda") if ra in (2, 13) else (op, rd, ra)
    if 32 <= op <= 55:
        return (op, "sda" if ra in (2, 13) else word & 0xFFFF)
    return word


def body(dol, address, limit=40):
    out = []
    for index in range(limit):
        word = dol.word(address + 4 * index)
        if word is None:
            break
        out.append(normalize(word))
        if word == BLR or ((word >> 26) == 18 and (word & 1) == 0):
            break
    return out


def similarity(a, b):
    common = sum((Counter(a) & Counter(b)).values())
    return common / max(len(a), len(b), 1)


def check_dols(rows, usa, jpn, window=0x400):
    errors = []
    for first, last, delta in rows:
        if first != last or not usa.is_text(first) or not is_start(usa, first):
            continue
        target = first + delta
        reference = body(usa, first)
        current = similarity(reference, body(jpn, target)) if is_start(jpn, target) else -1.0
        best, best_address = current, target
        for step in range(-window, window + 4, 4):
            candidate = target + step
            if candidate != target and is_start(jpn, candidate):
                score = similarity(reference, body(jpn, candidate))
                if score > best + 0.001:
                    best, best_address = score, candidate
        if current < 0:
            errors.append(f"0x{first:08X} -> 0x{target:08X} is not a function start in R2VJ01"
                          f" (closest body match 0x{best_address:08X}, {best:.2f})")
        elif best_address != target:
            errors.append(f"0x{first:08X} -> 0x{target:08X} matches {current:.2f}, but 0x{best_address:08X}"
                          f" matches {best:.2f}")
    return errors


SDA_BASES = {"usa": (0x804B7700, 0x804BA920), "jpn": (0x80468320, 0x8046B4E0)}  # r13, r2
SDA_RANGE = (0x804AF700, 0x804BB000)  # R2VE01 .sdata .. .sbss2 end, both small-data bases
SDA_OPS = {14} | set(range(32, 46)) | set(range(48, 56))


def sda_target(word, region):
    op, base = word >> 26, (word >> 16) & 31
    if op not in SDA_OPS or base not in (13, 2):
        return None
    offset = word & 0xFFFF
    offset = offset - 0x10000 if offset & 0x8000 else offset
    return (SDA_BASES[region][0 if base == 13 else 1] + offset) & 0xFFFFFFFF


def function_start(dol, address):
    for step in range(4000):
        if is_start(dol, address - 4 * step):
            return address - 4 * step
    return address


def function_words(dol, start, limit=400):
    words = []
    for index in range(limit):
        word = dol.word(start + 4 * index)
        if word is None:
            break
        words.append(word)
        if word == BLR and index and is_start(dol, start + 4 * (index + 1)):
            break
    return words


def check_sda_rows(rows, usa, jpn, window=0x1000):
    """A small-data row must be the variable the matching Japanese function accesses: the k-th
    same-opcode r13/r2 access of the USA function and of its best-matching Japanese function."""
    code_rows = sorted((first, delta) for first, last, delta in rows if first == last and usa.is_text(first))
    sites = {}
    for address in range(0x80004000, usa.sections[1][1], 4):
        word = usa.word(address)
        target = sda_target(word, "usa") if word is not None else None
        if target is not None:
            sites.setdefault(target, []).append(address)
    errors = []
    for first, last, delta in rows:
        if first != last or not SDA_RANGE[0] <= first < SDA_RANGE[1]:
            continue
        votes = Counter()
        for site in sites.get(first, [])[:8]:
            usa_start = function_start(usa, site)
            nearest = min(code_rows, key=lambda row: abs(row[0] - usa_start))[1]
            reference = body(usa, usa_start, 60)
            score, jpn_start = 0.0, None
            for step in range(-window, window + 4, 4):
                candidate = usa_start + nearest + step
                if is_start(jpn, candidate):
                    s = similarity(reference, body(jpn, candidate, 60))
                    if s > score:
                        score, jpn_start = s, candidate
            if jpn_start is None or score < 0.6:
                continue
            op, base = usa.word(site) >> 26, (usa.word(site) >> 16) & 31
            same = lambda words, region: [sda_target(w, region) for w in words
                                          if w >> 26 == op and (w >> 16) & 31 == base and sda_target(w, region) is not None]
            usa_same = same(function_words(usa, usa_start), "usa")
            jpn_same = same(function_words(jpn, jpn_start), "jpn")
            if len(usa_same) == len(jpn_same) and first in usa_same:
                votes[jpn_same[usa_same.index(first)]] += 1
        if votes:
            voted = votes.most_common(1)[0][0]
            if voted != first + delta:
                errors.append(f"small-data 0x{first:08X} -> 0x{first + delta:08X}, but the matching R2VJ01 code"
                              f" accesses 0x{voted:08X}")
    return errors


def find_dol(candidates):
    for candidate in candidates:
        path = Path(candidate)
        if path.is_file():
            return path
    return None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--usa-dol")
    parser.add_argument("--jpn-dol")
    args = parser.parse_args(argv)
    rows = load_table()
    errors = check_projects(rows, ROOT / "projects/r2ve01/recomp.yml", ROOT / "projects/r2vj01/recomp.yml")
    usa_dol = find_dol([args.usa_dol] if args.usa_dol else [ROOT / "orig/R2VE01/sys/main.dol"])
    jpn_dol = find_dol([args.jpn_dol] if args.jpn_dol else [ROOT / "orig/R2VJ01/sys/main.dol"])
    if usa_dol and jpn_dol:
        usa, jpn = Dol(usa_dol), Dol(jpn_dol)
        errors += check_dols(rows, usa, jpn)
        errors += check_sda_rows(rows, usa, jpn)
        print(f"checked {len(rows)} rows against {usa_dol} and {jpn_dol}")
    else:
        print("main.dol files not found; checked project address lists only")
    for error in errors:
        print("ERROR", error)
    print("PASS" if not errors else f"FAIL ({len(errors)})")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
