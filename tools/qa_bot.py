#!/usr/bin/env python3
"""Generate a SIN_VREMOTE_SCRIPT for an automated 'fighting' player.
Usage: python tools/qa_bot.py [--seed N] [--seconds 120] [--style fight|menu|idle] [--pause-every S]
Holds fire (B), wiggles aim/move sticks randomly, jumps (C), dodges (Z), taps A now and then to advance
result / continue / game-over screens, optionally taps PLUS to open/close the pause menu. Ends with 'loop'."""
import argparse, random

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--seconds', type=float, default=120)
    ap.add_argument('--style', default='fight')
    ap.add_argument('--a-every', type=float, default=7.0, help='seconds between A taps (advance screens)')
    ap.add_argument('--pause-every', type=float, default=0.0, help='seconds between PLUS pause/unpause (0=never)')
    ap.add_argument('--lead', type=float, default=30.0, help='idle seconds before the bot starts (boot/menu is script-driven by A taps)')
    a = ap.parse_args()
    r = random.Random(a.seed)
    segs = []
    t = 0.0
    nextA = a.a_every
    nextPause = a.pause_every if a.pause_every > 0 else 1e9
    def f(v): return ('%.2f' % v).rstrip('0').rstrip('.') or '0'
    while t < a.seconds:
        if t < a.lead:
            segs.append('0.2:A'); segs.append('2.8'); t += 3.0; continue
        if t >= nextPause:
            segs.append('0.25:PLUS'); segs.append('1.5'); segs.append('0.25:PLUS'); segs.append('1')
            t += 3.0; nextPause = t + a.pause_every; continue
        if t >= nextA:
            segs.append('0.15:A'); segs.append('0.3'); t += 0.45; nextA = t + a.a_every * r.uniform(0.7, 1.3); continue
        d = r.uniform(0.3, 0.9)
        rx, ry = r.uniform(-1, 1), r.uniform(-1, 1)
        lx, ly = r.uniform(-1, 1), r.uniform(-1, 1)
        btn = []
        if a.style == 'fight':
            if r.random() < 0.85: btn.append('B')
            if r.random() < 0.12: btn.append('C')
            if r.random() < 0.12: btn.append('Z')
        elif a.style == 'idle':
            pass
        segs.append('%s:%s:%s/%s:%s/%s' % (f(d), '+'.join(btn), f(rx), f(ry), f(lx), f(ly)))
        t += d
    segs.append('loop')
    print('|'.join(segs))

main()

