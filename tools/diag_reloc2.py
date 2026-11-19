import sys, struct
sys.path.insert(0, 'tools')
from pathlib import Path
from sdk_match3 import elf_functions
from sdk_globals import read_dol_text

ta, tl, text = read_dol_text(Path('orig/R2VE01/sys/main.dol'))
tw = struct.unpack_from(f'>{tl//4}I', text, 0)
lib = Path(r'build/sdk/x/wii_development_package/RVL_SDK/RVL/lib/vi.a')
a_body = {f: (t, m) for f, t, m in elf_functions(lib)}

f = 'VIWaitForRetrace'
t, m = a_body[f]
sin = 0x801BB130
p0 = (sin - ta) // 4
print(f'{f}: .a {len(t)} bytes; reloc-masked {sorted(m)}')
for i in range(len(t) // 4):
    aw = struct.unpack_from('>I', t, i * 4)[0]
    sw = tw[p0 + i]
    mark = '*' if aw != sw else ' '
    print(f'  +{i*4:04x} {mark} .a {aw:08X}  sin {sw:08X}  (op {aw>>26:02X} ra {(aw>>16)&0x1F:02X})')
