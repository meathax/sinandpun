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

POISON = 0xDEAD0000


def parse_dol_layout(path):
    """Return the SEL logical-section base addresses for a main.dol.

    SEL indices use the linker section names rather than DOL section-table
    indices.  The mapping is fixed for these CodeWarrior Wii DOLs; deriving it
    here keeps the linker independent of the R2VE01 DOL's section-table order.
    """
    d = open(path, 'rb').read()
    addrs = struct.unpack_from('>18I', d, 0x48)
    sizes = struct.unpack_from('>18I', d, 0x90)
    bss_addr, bss_size = struct.unpack_from('>2I', d, 0xD8)
    bases = {
        '.init': addrs[0],
        '.text': addrs[1],
        '.ctors': addrs[9],
        '.dtors': addrs[10],
        '.rodata': addrs[11],
        '.data': addrs[12],
        '.bss': bss_addr,
        '.sdata': addrs[13],
        '.sdata2': addrs[14],
        # The zero-filled small-data sections begin immediately after their
        # initialized counterparts in the link layout.
        '.sbss': addrs[13] + sizes[13],
        '.sbss2': addrs[14] + sizes[14],
    }
    return bases, addrs[8]


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
    rso_p, dol_p, sel_p, base_s, outp = sys.argv[1:6]
    base = int(base_s, 16)
    m = parse(rso_p)
    sel = parse(sel_p)
    dol_bases, eti_base = parse_dol_layout(dol_p)
    sel_syms = {}
    for name, si, off in sel['exports']:
        if si == ABS:
            a = off
        elif si == ETI:
            a = eti_base + off
        else:
            sn = DOL_SECTION_NAMES[si]
            assert sn, (name, si)
            a = dol_bases[sn] + off
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

    # The linker map names exported symbols, but several indirect call targets are
    # purely local to the module: static-constructor table entries, local vtable
    # methods, and compiler-generated adjustor thunks. The translator consumes this
    # map as a function-boundary oracle, so augment it with evidence-based entries
    # using the same shapes as tools/gen_region_function_map.py. RSO section 3 is
    # the text section in these modules (the executable flag bit is not reliable).
    exec_indices = {m['prolog'][0]} if m['prolog'][0] in sec_addr else set()
    exec_indices.update(i for i, sec in enumerate(m['secs']) if sec.get('exec') and i in sec_addr)
    exec_ranges = [(sec_addr[i], m['secs'][i]['size']) for i in sorted(exec_indices)]

    def img_word(address):
        off = address - base
        if 0 <= off and off + 4 <= len(img):
            return u32(img, off)
        raise KeyError(hex(address))

    def in_text(address):
        return any(lo <= address < lo + size for lo, size in exec_ranges)

    def frame_prologue(word):
        return (word & 0xFFFF0000) == 0x94210000 and (word & 0x8000) != 0

    text_lo = min((lo for lo, _ in exec_ranges), default=base)

    def looks_like_entry(address):
        if not in_text(address):
            return False
        try:
            if frame_prologue(img_word(address)):
                return True
            probe = address - 4
            while probe >= text_lo and in_text(probe) and img_word(probe) == 0:
                probe -= 4
            terminator = img_word(probe)
            return terminator in (0x4E800020, 0x4E800420)  # blr / bctr
        except KeyError:
            return False

    inferred = set()
    for lo, size in exec_ranges:
        for address in range(lo, lo + size - 8, 4):
            try:
                word = img_word(address)
                next_word = img_word(address + 4)
            except KeyError:
                continue
            # Frame prologues are real entry points. They are needed even when no
            # data word references them: several module factories keep method or
            # constructor pointers in generated code rather than a simple vtable
            # word; such functions are reached only indirectly from gameplay.
            if frame_prologue(word):
                inferred.add(address)
            # addi r3,r3,-N ; b implementation: virtual-base adjusting thunk.
            if ((word & 0xFFFF0000) == 0x38630000 and
                    (next_word & 0xFC000003) == 0x48000000):
                inferred.add(address)
    for i, sec in enumerate(m['secs']):
        if i in exec_indices or i not in sec_addr or not sec['size']:
            continue
        lo = sec_addr[i]
        for address in range(lo, lo + sec['size'] - 4, 4):
            try:
                target = img_word(address)
            except KeyError:
                continue
            if target % 4 == 0 and in_text(target) and looks_like_entry(target):
                inferred.add(target)

    mapped_addresses = set(symmap.values())
    added_inferred = 0
    for address in sorted(inferred):
        if address not in mapped_addresses:
            symmap[f'fn_{address:08X}'] = address
            mapped_addresses.add(address)
            added_inferred += 1

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
          f"relocs int={len(m['irel'])} ext={len(m['erel'])} unresolved={len(unresolved)} {sorted(unresolved)[:3]} missing_sec_relocs={len(missing)} "
          f"inferred_functions={added_inferred}")


main()
