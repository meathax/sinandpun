import sys, struct
sys.path.insert(0, 'tools')
from pathlib import Path
from sdk_match3 import elf_functions
from sdk_globals import read_dol_text

ta, tl, text = read_dol_text(Path('orig/R2VE01/sys/main.dol'))
tw = struct.unpack_from(f'>{tl//4}I', text, 0)
lib = Path(r'build/sdk/x/wii_development_package/RVL_SDK/RVL/lib/vi.a')
a_body = {f: (t, m) for f, t, m in elf_functions(lib)}
for fname, sin, roff in (('VIWaitForRetrace', 0x801BB130, 0x12ec), ('VIFlush', 0x801BC0F0, 0x237c)):
    t, m = a_body[fname]
    p = (sin - ta) // 4 + roff // 4
    aw = struct.unpack_from('>I', t, roff)[0]
    print(f'{fname} +{roff:#x}: a_size={len(t):#x} masked_sample={sorted(m)[:8]}')
    print(f'  .a word {aw:08X}  sin word {tw[p]:08X} (op={tw[p]>>26:02X} ra={(tw[p]>>16)&0x1F})')
