"""Converts premultiplied RGBA dumps from ui_preview into PNGs composited over a background screenshot."""
import glob
import os
import struct
import sys

from PIL import Image

scratch, prefix, background, out = sys.argv[1:5]
for path in sorted(glob.glob(os.path.join(scratch, prefix + "_*.rgba"))):
    with open(path, "rb") as f:
        w, h = struct.unpack("ii", f.read(8))
        data = f.read()
    premult = Image.frombytes("RGBA", (w, h), data)
    if os.path.exists(background):
        bg = Image.open(background).convert("RGB").resize((w, h), Image.LANCZOS)
    else:
        bg = Image.new("RGB", (w, h), (20, 60, 70))
    # out = premult.rgb + bg * (1 - a)
    r, g, b, a = premult.split()
    inv = a.point(lambda v: 255 - v)
    base = Image.composite(Image.new("RGB", (w, h), (0, 0, 0)), bg, Image.new("L", (w, h), 0))
    bg_scaled = Image.merge("RGB", [ch.point(lambda v: v) for ch in bg.split()])
    from PIL import ImageChops
    bg_part = ImageChops.multiply(bg_scaled, Image.merge("RGB", [inv, inv, inv]))
    result = ImageChops.add(Image.merge("RGB", [r, g, b]), bg_part)
    name = os.path.basename(path)[:-5] + ".png"
    result.save(os.path.join(out, name))
    print("saved", os.path.join(out, name))
