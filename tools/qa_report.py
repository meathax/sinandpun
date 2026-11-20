#!/usr/bin/env python3
"""Summarise a build/qa/<name>/err.log: perf stalls, working set, flow events, fatals, audio underruns."""
import re, sys, os
d = sys.argv[1] if os.path.isdir(sys.argv[1]) else os.path.join('build', 'qa', sys.argv[1])
lines = open(os.path.join(d, 'err.log'), errors='replace').read().splitlines()
fps, ws, low = [], [], []
pat_bad = re.compile(r'FATAL|Runtime error|not translated|Access|unhandled|abort|OSFatal|crash|sound-slot-guard', re.I)
ev = []
und = 0
t = 0
for l in lines:
    m = re.search(r'\[perf\] fps=([\d.]+) guest=([\d.]+).*?ws=(\d+)MB', l)
    if m:
        t += 1
        g = float(m.group(2))
        fps.append(g)
        ws.append(int(m.group(3)))
        if g < 50:
            low.append((t, g))
    if re.search(r'audio-dbg.*underrun', l, re.I):
        und += 1
    if re.search(r'autostage\] t=\S+ (F state|stage|forced)|RSOLinkListFixed|RSOUnLinkList|scene=.*(GAMEOVER|RESULT|CONT|SAVE)|gover.brstm off=0x0 len=0x40$|sound-slot|DANGLING', l):
        ev.append(l[:170])
    elif pat_bad.search(l) and 'PARKING' not in l:
        ev.append('!! ' + l[:200])
print(f'{d}: perf samples={len(fps)} ws first/last/max={ws[:1]}/{ws[-1:]}/{max(ws) if ws else 0} MB, underruns={und}')
if fps:
    s = sorted(fps)
    print(f'guest fps min={s[0]:.1f} p1={s[len(s)//100]:.1f} median={s[len(s)//2]:.1f}; seconds<50: {len(low)} {low[:12]}')
for e in ev[:int(sys.argv[2]) if len(sys.argv) > 2 else 60]:
    print(e)
