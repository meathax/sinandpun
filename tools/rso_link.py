#!/usr/bin/env python3
"""Statically link a Wii RSO module at a fixed guest base into a flat image.

Usage: rso_link.py <module.rso> <main.dol> <gs2.sel> <base_hex> <out_prefix>
Writes <out_prefix>.bin (image at <base>), .map ("hexaddr name") and .json.
Format reference: decomp-toolkit src/util/rso.rs. Imports resolve by name through the SEL
(main.dol export list); SEL section indices are hardcoded DOL section ids.
"""
import json
import struct
import sys

DOL_SECTION_NAMES = [None, '.init', '.text', '.ctors', '.dtors', '.rodata', '.data', '.bss',
                     '.sdata', '.sdata2', None, '.sbss', '.sbss2', None]
ETI, ABS = 241, 65521
R_NONE, R_ADDR32, R_ADDR16_LO, R_ADDR16_HI, R_ADDR16_HA, R_REL24, R_REL14, R_UADDR32 = 0, 1, 4, 5, 6, 10, 11, 24

# main.dol section bases (dtk dol info, R2VE01)
DOL_BASES = {'.init': 0x80004000, '.text': 0x8001F240, '.ctors': 0x802D5100, '.dtors': 0x802D5240,
             '.rodata': 0x802D5280, '.data': 0x802F5260, '.bss': 0x80334680, '.sdata': 0x804AF700,
             '.sdata2': 0x804B2920, '.sbss': 0x804B1960, '.sbss2': 0x804B67E0}
ETI_BASE = 0x800174A0
POISON = 0xDEAD0000


def u32(d, o):
    return struct.unpack_from('>I', d, o)[0]


def cstr(d, o):
    e = d.index(b'\x00', o)
    return d[o:e].decode('ascii')


def parse(path):
    d = open(path, 'rb').read()
    nsec, sec_off, name_off, _name_sz, ver, bss_sz = (u32(d, 8 + 4 * i) for i in range(6))
    prolog_sec, epilog_sec, unres_sec = d[0x20], d[0x21], d[0x22]
    prolog_off, epilog_off, unres_off = u32(d, 0x24), u32(d, 0x28), u32(d, 0x2C)
    irel_off, irel_sz, erel_off, erel_sz = (u32(d, 0x30 + 4 * i) for i in range(4))
    ex_off, ex_sz, ex_nm, im_off, im_sz, im_nm = (u32(d, 0x40 + 4 * i) for i in range(6))
    assert ver == 1
    secs = []
    for i in range(nsec):
        o, s = u32(d, sec_off + 8 * i), u32(d, sec_off + 8 * i + 4)
        secs.append({'off': o & ~1, 'exec': bool(o & 1), 'size': s})
    exports = []
    for i in range(ex_sz // 16):
        no, off, si, _hs = struct.unpack_from('>4I', d, ex_off + 16 * i)
        exports.append((cstr(d, ex_nm + no), si, off))
    imports = []
    for i in range(im_sz // 12):
        no, _off, _si = struct.unpack_from('>3I', d, im_off + 12 * i)
        imports.append(cstr(d, im_nm + no))

    def relocs(off, size):
        return [struct.unpack_from('>3I', d, off + 12 * i) for i in range(size // 12)]

    return dict(data=d, secs=secs, exports=exports, imports=imports,
                irel=relocs(irel_off, irel_sz), erel=relocs(erel_off, erel_sz),
                prolog=(prolog_sec, prolog_off), epilog=(epilog_sec, epilog_off),
                unres=(unres_sec, unres_off), bss=bss_sz,
                name=cstr(d, name_off) if name_off else '')


def main():
    rso_p, _dol_p, sel_p, base_s, outp = sys.argv[1:6]
    base = int(base_s, 16)
    m = parse(rso_p)
    sel = parse(sel_p)
    sel_syms = {}
    for name, si, off in sel['exports']:
        if si == ABS:
            a = off
        elif si == ETI:
            a = ETI_BASE + off
        else:
            sn = DOL_SECTION_NAMES[si]
            assert sn, (name, si)
            a = DOL_BASES[sn] + off
        sel_syms[name] = a
    # layout: file-backed sections first, then bss; 32-byte aligned
    addr = base
    sec_addr = {}
    for i, s in enumerate(m['secs']):
        if s['size'] and s['off']:
            addr = (addr + 31) & ~31
            sec_addr[i] = addr
            s['addr'] = addr
            addr += s['size']
    for i, s in enumerate(m['secs']):
        if s['size'] and not s['off']:
            addr = (addr + 31) & ~31
            sec_addr[i] = addr
            s['addr'] = addr
            addr += s['size']
    end = addr
    img = bytearray(end - base)
    d = m['data']
    for s in m['secs']:
        if s['size'] and s['off']:
            img[s['addr'] - base: s['addr'] - base + s['size']] = d[s['off']: s['off'] + s['size']]

    def locate(fo):
        for s in m['secs']:
            if s['size'] and s['off'] and s['off'] <= fo < s['off'] + s['size']:
                return s['addr'] + fo - s['off']
        raise ValueError('reloc outside sections %x' % fo)

    unresolved = set()
    missing = []
    counts = {}

    stubs = {}  # target -> stub address (far-jump stubs for REL24 beyond +-32 MB)
    stub_base = (end + 31) & ~31

    def far_stub(target):
        # lis r0,hi ; ori r0,r0,lo ; mtctr r0 ; bctr  (r0 is never an argument register; r12 must survive: the MWCC __ptmf_scall thunk takes the PTMF pointer in r12)
        if target not in stubs:
            at = stub_base + 16 * len(stubs)
            stubs[target] = at
            need = at + 16 - base
            if len(img) < need:
                img.extend(bytes(need - len(img)))
            struct.pack_into('>IIII', img, at - base, 0x3C000000 | ((target >> 16) & 0xFFFF),
                             0x60000000 | (target & 0xFFFF), 0x7C0903A6, 0x4E800420)
        return stubs[target]

    def apply(loc, val, ty):
        o = loc - base
        counts[ty] = counts.get(ty, 0) + 1
        if ty == R_REL24 and not (-0x2000000 <= val - loc < 0x2000000):
            val = far_stub(val)
        if ty in (R_ADDR32, R_UADDR32):
            struct.pack_into('>I', img, o, val & 0xFFFFFFFF)
        elif ty == R_ADDR16_LO:
            struct.pack_into('>H', img, o, val & 0xFFFF)
        elif ty == R_ADDR16_HI:
            struct.pack_into('>H', img, o, (val >> 16) & 0xFFFF)
        elif ty == R_ADDR16_HA:
            struct.pack_into('>H', img, o, ((val + 0x8000) >> 16) & 0xFFFF)
        elif ty == R_REL24:
            w = u32(img, o)
            struct.pack_into('>I', img, o, (w & 0xFC000003) | ((val - loc) & 0x3FFFFFC))
        elif ty == R_REL14:
            w = u32(img, o)
            struct.pack_into('>I', img, o, (w & 0xFFFF0003) | ((val - loc) & 0xFFFC))
        else:
            raise ValueError('unhandled reloc type %d' % ty)

    for off, idt, tgt in m['irel']:
        ty = idt & 0xFF
        if ty != R_NONE:
            sid = idt >> 8
            if sid not in sec_addr:
                # relocation against an empty/discarded section (dtk-style leftovers): point at a poison address
                missing.append((locate(off), sid, tgt))
                apply(locate(off), POISON + tgt, ty)
                continue
            apply(locate(off), sec_addr[sid] + tgt, ty)
    for off, idt, _tgt in m['erel']:
        ty = idt & 0xFF
        if ty == R_NONE:
            continue
        name = m['imports'][idt >> 8]
        if name not in sel_syms:
            unresolved.add(name)
            continue
        apply(locate(off), sel_syms[name], ty)
    symmap = {}
    skipped_exports = []
    for target, at in stubs.items():
        symmap[f'farjump_{target:08X}'] = at
    if stubs:
        end = stub_base + 16 * len(stubs)
    for name, si, off in m['exports']:
        if si not in sec_addr:
            skipped_exports.append(name)  # debug-section pseudo symbols (.dwarf_*)
            continue
        symmap[name] = sec_addr[si] + off
    for tag, (si, off) in (('_prolog', m['prolog']), ('_epilog', m['epilog']), ('_unresolved', m['unres'])):
        if si:
            symmap[tag] = sec_addr[si] + off
    open(outp + '.bin', 'wb').write(img)
    with open(outp + '.map', 'w', newline='\n') as f:
        for n, a in sorted(symmap.items(), key=lambda x: x[1]):
            f.write(f'{a:08x} {n}\n')
    exec_secs = [[s['addr'], s['size']] for s in m['secs'] if s['exec'] and s['size']]
    json.dump(dict(name=m['name'], base=base, end=end,
                   sections={str(i): [s['addr'], s['size']] for i, s in enumerate(m['secs']) if s['size']},
                   exec=exec_secs, imports=len(m['imports']), unresolved=sorted(unresolved),
                   skipped_exports=len(skipped_exports), skipped_sample=skipped_exports[:5], reloc_counts=counts, missing_section_relocs=[[hex(a), s, hex(o)] for a, s, o in missing], prolog=symmap.get('_prolog'), epilog=symmap.get('_epilog'),
                   unresolved_fn=symmap.get('_unresolved')),
              open(outp + '.json', 'w'), indent=1)
    nm = m['name'].replace('\\', '/').split('/')[-1]
    print(f"{nm}: base {base:08X} end {end:08X} size {end - base:#x} exports {len(m['exports'])} "
          f"relocs int={len(m['irel'])} ext={len(m['erel'])} unresolved={len(unresolved)} {sorted(unresolved)[:3]} missing_sec_relocs={len(missing)}")


main()
