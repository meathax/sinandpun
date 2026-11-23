#!/usr/bin/env python3
"""Find function-start candidates in a pre-linked stage module image and add them to its map.

Seeds for the translator come from build/modules/stgNN.map. The translator treats EVERY map entry as a
function boundary, so false positives (e.g. switch case labels) split real functions, and misses are fatal
at runtime when the function is only reached through a pointer.

Sources (relocations are already applied in the .bin, so lis/addi pairs carry the real addresses):
  bl        direct call target inside text
  tail      `b` / `bc` (no link) whose target is outside the enclosing function span (iterated to fixpoint)
  data      aligned word in a non-text section pointing into text (vtables, fn-pointer tables, ctors/dtors)
  lisaddi   lis rX,hi ; addi/ori rY,rX,lo (within a few insns, rX not clobbered) resolving into text
  prologue  stwu r1,-N(r1) / mflr r0 after blr or a one-instruction tail-call thunk
  bctr      constant lis/ori + mtctr + bctr destination (also covered by lisaddi)
Jump tables (bctr after mtctr <- lwzx from a lis/addi table base) are recognised; their entries are
switch CASE LABELS, not functions, and are removed from the seed set (they stay inside the owning function).
Only candidates whose predecessor is a terminator (blr/b/bctr/trap/nop padding) or whose first insn is a
standard prologue are accepted as new seeds; the rest are reported as `suspect` and never written.

Usage: python tools/module_seeds.py stg00 [--write] [--verbose]
"""
import argparse
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BLR = 0x4E800020
TERMINATORS = (0x4E800020, 0x4E800420)


class Module:
    def __init__(self, name):
        self.name = name
        meta = json.load(open(ROOT / f"build/modules/{name}.json"))
        self.base = meta["base"]
        self.img = (ROOT / f"build/modules/{name}.bin").read_bytes()
        secs = {int(k): v for k, v in meta["sections"].items()}
        self.tbase, self.tsize = secs[3]
        self.tend = self.tbase + self.tsize
        self.data_secs = [(sid, b, s) for sid, (b, s) in sorted(secs.items()) if sid != 3 and b >= self.base]
        self.words = struct.unpack_from(f">{self.tsize // 4}I", self.img, self.tbase - self.base)
        self.map_path = ROOT / f"build/modules/{name}.map"
        self.map_addrs = set()
        self.map_text = set()
        for l in self.map_path.read_text().splitlines():
            p = l.split()
            if p:
                try:
                    a = int(p[0], 16)
                except ValueError:
                    continue
                self.map_addrs.add(a)
        # farjump stubs live after the sections; keep every map entry, text ones matter for boundaries
        self.map_text = {a for a in self.map_addrs if self.in_text(a)}

    def in_text(self, a):
        return self.tbase <= a < self.tend and a % 4 == 0

    def w(self, a):
        return self.words[(a - self.tbase) >> 2]

    def data_words(self):
        for sid, b, s in self.data_secs:
            off = b - self.base
            n = s // 4
            if n:
                vals = struct.unpack_from(f">{n}I", self.img, off)
                for i, v in enumerate(vals):
                    yield b + i * 4, v


def simm16(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def branch_target(w, pc):
    op = w >> 26
    if op == 18:
        li = w & 0x3FFFFFC
        if li & 0x2000000:
            li -= 0x4000000
        return (li if w & 2 else pc + li) & 0xFFFFFFFF, bool(w & 1), "b"
    if op == 16:
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        return (bd if w & 2 else pc + bd) & 0xFFFFFFFF, bool(w & 1), "bc"
    return None


def prologue_like(w):
    return (w >> 16) == 0x9421 or w == 0x7C0802A6 or (w >> 16) == 0x9421 or (w >> 26) == 47  # stwu/mflr/stmw


def is_terminator_or_pad(w):
    return w in TERMINATORS or w == 0 or w == 0x60000000 or (w >> 26) == 18 and not (w & 1) or w >> 26 == 3 or w == 0x7FE00008


def find_jump_tables(m):
    """Return (tables {base:(count,[targets])}, case_label_targets set)."""
    tables = {}
    labels = set()
    W = m.words
    n = len(W)
    for i, w in enumerate(W):
        if w != 0x4E800420:  # bctr
            continue
        # mtctr rT
        t = None
        for j in range(i - 1, max(i - 8, -1), -1):
            if W[j] & 0xFC1FFFFF == 0x7C0903A6:
                t = (W[j] >> 21) & 31
                mj = j
                break
        if t is None:
            continue
        # lwzx rT,rA,rB  (xo 23) feeding t
        lw = None
        for j in range(mj - 1, max(mj - 8, -1), -1):
            x = W[j]
            if x >> 26 == 31 and (x >> 1) & 0x3FF == 23 and (x >> 21) & 31 == t:
                lw = j
                break
            if x >> 26 == 32 and (x >> 21) & 31 == t:  # lwz rT,lo(rA): base+disp
                lw = j
                break
        if lw is None:
            continue
        x = W[lw]
        regs = [(x >> 16) & 31, (x >> 11) & 31] if x >> 26 == 31 else [(x >> 16) & 31]
        disp = simm16(x) if x >> 26 == 32 else 0
        base = None
        for r in regs:
            # find the constant materialised in r
            for j in range(lw - 1, max(lw - 40, -1), -1):
                y = W[j]
                op = y >> 26
                if op == 14 and (y >> 21) & 31 == r:  # addi r,ra,lo
                    ra = (y >> 16) & 31
                    for k in range(j - 1, max(j - 12, -1), -1):
                        z = W[k]
                        if z >> 26 == 15 and (z >> 21) & 31 == ra and (z >> 16) & 31 == 0:
                            base = ((z & 0xFFFF) << 16) + simm16(y)
                            break
                    break
                if op == 15 and (y >> 21) & 31 == r and (y >> 16) & 31 == 0:
                    base = (y & 0xFFFF) << 16
                    break
            if base is not None:
                break
        if base is None:
            continue
        base = (base + disp) & 0xFFFFFFFF
        # bound from cmplwi rI,N (+ bgt) in the preceding window; otherwise read plausible entries
        cnt = None
        for j in range(lw - 1, max(lw - 30, -1), -1):
            y = W[j]
            if y >> 26 == 10:  # cmpli
                cnt = (y & 0xFFFF) + 1
                break
        fstart_win = m.tbase + max(0, (i - 4096)) * 4
        ents = []
        off = base
        limit = cnt if cnt is not None else 1024
        for k in range(limit):
            a = base + 4 * k
            if not (m.base <= a < m.base + len(m.img) - 3):
                break
            v = struct.unpack_from(">I", m.img, a - m.base)[0]
            pc = m.tbase + i * 4
            if not (m.in_text(v) and abs(v - pc) < 0x10000):
                break
            ents.append(v)
        if ents and (cnt is None or len(ents) == cnt):
            tables[base] = (len(ents), ents)
            labels.update(ents)
    return tables, labels


def scan(m, verbose=False):
    W = m.words
    n = len(W)
    cands = {}  # addr -> set(sources)

    def add(a, src):
        if m.in_text(a):
            cands.setdefault(a, set()).add(src)

    tables, jt_labels = find_jump_tables(m)
    jt_slots = set()
    for b, (c, ents) in tables.items():
        for k in range(c):
            jt_slots.add(b + 4 * k)

    # direct calls and branches
    branches = []  # (pc, tgt, kind)
    for i, w in enumerate(W):
        pc = m.tbase + i * 4
        bt = branch_target(w, pc)
        if not bt:
            continue
        tgt, link, kind = bt
        if link:
            add(tgt, "bl")
        elif m.in_text(tgt):
            branches.append((pc, tgt, kind))

    # materialised addresses: lis rX,hi then addi/ori within a short window
    for i, w in enumerate(W):
        if w >> 26 != 15 or (w >> 16) & 31 != 0:
            continue
        r = (w >> 21) & 31
        hi = w & 0xFFFF
        for j in range(i + 1, min(i + 10, n)):
            y = W[j]
            op = y >> 26
            if op in (14, 24) and (y >> 16 if op == 14 else y >> 21) & 31 == r:
                if op == 14:
                    val = ((hi << 16) + simm16(y)) & 0xFFFFFFFF
                else:
                    val = ((hi << 16) | (y & 0xFFFF)) & 0xFFFFFFFF
                add(val, "lisaddi")
                break
            # clobber of r ends the window
            if op in (14, 15, 24, 32, 34, 40, 42, 46, 48, 50, 56) and (y >> 21) & 31 == r and op not in (24,):
                break
            if op == 31 and (y >> 21) & 31 == r and ((y >> 1) & 0x3FF) in (444, 316, 266, 40, 23, 87, 279, 339):
                break

    # A standalone tail-call thunk can also end immediately before a callback.
    for i in range(1, n):
        pc = m.tbase + i * 4
        previous = W[i - 1]
        thunk_boundary = pc - 4 in m.map_text or (i >= 2 and W[i - 2] == BLR)
        tail_thunk = thunk_boundary and previous >> 26 == 18 and not previous & 1
        if (previous == BLR or tail_thunk) and ((W[i] >> 16) == 0x9421 or W[i] == 0x7C0802A6):
            add(pc, "prologue")

    # data pointers (jump-table slots recognised above are case labels, not pointers)
    for a, v in m.data_words():
        if m.in_text(v) and a not in jt_slots:
            add(v, "data")

    # tail calls: iterate with the growing entry set
    entries = set(m.map_text) | {a for a, s in cands.items() if s - {"lisaddi"}}
    entries |= {a for a, s in cands.items() if "lisaddi" in s}
    for _ in range(4):
        import bisect
        es = sorted(entries)
        new = 0
        for pc, tgt, kind in branches:
            k = bisect.bisect_right(es, pc) - 1
            lo = es[k] if k >= 0 else m.tbase
            hi = es[k + 1] if k + 1 < len(es) else m.tend
            if lo <= tgt < hi or tgt in entries or tgt in jt_labels:
                continue
            pw = m.w(tgt - 4) if tgt > m.tbase else BLR
            if (pw in TERMINATORS or pw == 0) and prologue_like(m.w(tgt)):
                add(tgt, "tail")
                entries.add(tgt)
                new += 1
        if not new:
            break
    return cands, jt_labels, tables


def classify(m, a, srcs, jt_labels):
    """Return 'ok' or a suspect reason."""
    if a in jt_labels and not (srcs & {"bl", "prologue"}):
        return "jt-case"
    w = m.w(a)
    pw = m.w(a - 4) if a > m.tbase else BLR
    if "bl" in srcs and (w >> 26) != 0:
        return "ok"
    if w == 0 or (w >> 26) in (0, 1, 2, 5, 6, 9, 22, 30, 57, 58, 61, 62):
        return "bad-first-insn"
    weak = not (srcs - {"lisaddi", "tail"})
    if pw in (BLR, 0):
        return "ok"
    if weak:
        return "ok" if prologue_like(w) and is_terminator_or_pad(pw) else "mid-function"
    if pw in TERMINATORS or prologue_like(w) or is_terminator_or_pad(pw):
        return "ok"
    return "mid-function"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--prune-jt", action="store_true", help="with --write: drop sub_ entries that are only switch case labels")
    ap.add_argument("--translated", help="ignored (kept for compatibility)")
    a = ap.parse_args()
    m = Module(a.name)
    cands, jt_labels, tables = scan(m)
    by = {}
    for c, s in cands.items():
        by.setdefault(classify(m, c, s, jt_labels), []).append(c)
    new = sorted(c for c in by.get("ok", []) if c not in m.map_addrs)
    src_new = {}
    for c in new:
        for s in cands[c]:
            src_new[s] = src_new.get(s, 0) + 1
    print(f"{a.name}: text 0x{m.tbase:08X}+0x{m.tsize:X}, {len(cands)} candidates, {len(m.map_addrs)} in map, "
          f"{len(tables)} jump tables ({len(jt_labels)} case labels), {len(new)} NEW seeds {src_new}")
    for k, v in sorted(by.items()):
        if k != "ok":
            print(f"  rejected {k}: {len(v)}  ({sum(1 for c in v if c in m.map_addrs)} of those already in map)")
    inmap_bad = [c for c in m.map_text if c in jt_labels and not (cands.get(c, set()) & {"bl", "prologue"})]
    if inmap_bad:
        print(f"  {len(inmap_bad)} existing map entries are jump-table case labels (split functions; use --prune-jt --write): "
              + " ".join(f"{c:08X}" for c in sorted(inmap_bad)[:8]))
    if a.verbose:
        for c in new:
            print(f"   {c:08X} {','.join(sorted(cands[c]))} first={m.w(c):08X}")
    # sub_ map entries that are only switch case labels: they split the owning function (the translator
    # then cannot recognise the jump table because labels fall outside the shrunken function window)
    prune = {c for c in inmap_bad if not (cands.get(c, set()) & {"tail", "lisaddi", "data"})}
    names = {}
    for l in m.map_path.read_text().splitlines():
        p = l.split()
        if len(p) > 1:
            names[int(p[0], 16)] = p[1]
    prune = {c for c in prune if names.get(c, "").startswith("sub_")}
    if a.prune_jt:
        print(f"  prunable jump-table case-label sub_ entries: {len(prune)}")
    if (a.write and new) or (a.prune_jt and prune):
        orig = m.map_path.with_suffix(".map.orig")
        if not orig.exists():
            orig.write_bytes(m.map_path.read_bytes())
        lines = m.map_path.read_text().splitlines()
        if a.prune_jt:
            lines = [l for l in lines if not (l.split() and int(l.split()[0], 16) in prune)]
        if a.write:
            lines += [f"{c:08x} sub_{c:08X}" for c in new]
        with open(m.map_path, "w", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        print(f"  map rewritten: +{len(new) if a.write else 0} seeds, -{len(prune) if a.prune_jt else 0} case labels")


if __name__ == "__main__":
    main()
