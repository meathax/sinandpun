"""Pixel statistics for a game-window screenshot, for agents that cannot view images.

Usage: python tools/shot_stats.py build/screenshots/x.png [--map]
Prints: size, distinct colours, top colours, bounding box of non-black content (luminance > 40)
and the fraction of non-black pixels. --map also prints a coarse ASCII luminance map.
Reference: a blank game window is ~338 colours, >90% pure black (0,0,0) with a grey title bar.
A rendered game frame has thousands of colours and a large non-black bounding box.
"""
import sys
from PIL import Image

path = sys.argv[1]
im = Image.open(path).convert("RGB")
w, h = im.size
cols = im.getcolors(1 << 24)
cols.sort(reverse=True)
lum = im.convert("L")
px = lum.load()
xs, ys, n = [], [], 0
for y in range(h):
    for x in range(w):
        if px[x, y] > 40:
            xs.append(x)
            ys.append(y)
            n += 1
print(f"size={w}x{h} colours={len(cols)} top={cols[:3]}")
print(f"non-black pixels={n} ({100.0 * n / (w * h):.1f}%) bbox={(min(xs), min(ys), max(xs), max(ys)) if xs else None}")
if "--map" in sys.argv:
    for yy in range(0, h, max(1, h // 24)):
        print("".join(" .:-=+*#%@"[min(9, px[x, yy] // 26)] for x in range(0, w, max(1, w // 80))))
