"""Join [vmouse] and [aim] lines of one run (build/mouse/<name>/err.log) and print per-segment settled readings.
usage: mouse_analyze.py NAME [--all]
Per segment (the last 25 % of its samples = settled): window fraction wanted, picture fraction expected from the real
presented viewport, game pointer (kpos, KPAD -1..1) and the error in % of the half picture.
"""
import re
import sys
import statistics as st

name = sys.argv[1]
show_all = '--all' in sys.argv
path = f'D:/Arcade/AI/aCORES/Sin/build/mouse/{name}/err.log'
kv = re.compile(r'(\w[\w.]*)=(\([^)]*\)|\S+)')


def parse(line):
    d = {}
    for k, v in kv.findall(line):
        d[k] = v
    return d


def pair(v):
    v = v.strip('()')
    a = v.split(',')
    return float(a[0]), float(a[1])


rows = []
marks = {}
last_aim = None
pending = []
for line in open(path, errors='replace'):
    if '[vmouse] t=' in line and ' segment ' in line:
        m = re.search(r'segment (\d+) start(?: mark=(\S+))?', line)
        if m:
            marks[int(m.group(1))] = m.group(2) or ''
    elif '[vmouse] t=' in line and 'want=' in line:
        pending.append(parse(line))
    elif line.startswith('[aim] '):
        a = parse(line)
        for v in pending:
            rows.append((v, a))
        pending = []

segs = {}
for v, a in rows:
    segs.setdefault(int(v['seg']), []).append((v, a))

print(f'{"seg":>3} {"mark":<14} {"want(fx,fy)":>13} {"exp(u,v)":>13} {"kpos(x,y)":>15} {"gpos(x,y)":>15} {"err%x":>6} {"err%y":>6} '
      f'{"cam":>13} sch dev dpd win vp')
for s in sorted(segs):
    items = segs[s]
    tail = items[max(0, int(len(items) * 0.75)):] if not show_all else items
    if not tail:
        continue
    v, a = tail[-1]
    fx, fy = (float(x) for x in v['want'].split(','))
    vp = v['vp'].split(':')
    vl, vt, vw, vh = (float(x) for x in vp[1].split(','))
    if vp[0] == '0':
        vl, vt, vw, vh = 0, 0, 1, 1
    u = min(1, max(0, (fx - vl) / vw)) if vw else 0.5
    w = min(1, max(0, (fy - vt) / vh)) if vh else 0.5
    kx = ky = gx = gy = float('nan')
    if 'kpos' in a:
        kx, ky = pair(a['kpos'])
        gx, gy = pair(a['gpos'])
    ex, ey = 2 * u - 1, 2 * w - 1
    errx = (kx - ex) * 100
    erry = (ky - ey) * 100
    cam = a.get('cam', '')
    print(f'{s:>3} {marks.get(s, ""):<14} {fx:6.3f},{fy:6.3f} {u:6.3f},{w:6.3f} {kx:7.3f},{ky:7.3f} {gx:7.3f},{gy:7.3f} '
          f'{errx:6.1f} {erry:6.1f} {cam:>13} {v["scheme"][:2]} {v["dev"]:<4} {a.get("dpd", "?")} {v["win"]}/{v["pix"]} {v["vp"]}')
