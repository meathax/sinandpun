"""Draws simple original line-art input-device badges (RGBA 3840x2160 overlays) for the trailer.

Each badge sits lower-left: a rounded dark plate, a cyan line icon and a label. Generic shapes only.
Usage: python tools/trailer_icons.py OUTDIR
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 3840, 2160
S = 4  # supersample
CYAN = (60, 220, 255, 255)
WHITE = (245, 248, 255, 255)


def plate_icon(kind, text):
    big = Image.new("RGBA", (W * S // 2, H * S // 2), (0, 0, 0, 0))  # work at 2x of quarter region
    img = Image.new("RGBA", (1500 * S, 420 * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, 1500 * S - 1, 420 * S - 1), radius=60 * S, fill=(6, 12, 24, 215),
                        outline=(60, 220, 255, 200), width=5 * S)
    lw = 14 * S
    ox, oy = 60 * S, 60 * S  # icon box 300x300
    u = S

    def R(x0, y0, x1, y1, r=20):
        d.rounded_rectangle((ox + x0 * u, oy + y0 * u, ox + x1 * u, oy + y1 * u), radius=r * u, outline=CYAN, width=lw)

    def C(x, y, r, fill=False):
        box = (ox + (x - r) * u, oy + (y - r) * u, ox + (x + r) * u, oy + (y + r) * u)
        if fill:
            d.ellipse(box, fill=CYAN)
        else:
            d.ellipse(box, outline=CYAN, width=lw)

    def L(*pts):
        d.line([(ox + x * u, oy + y * u) for x, y in pts], fill=CYAN, width=lw, joint="curve")

    if kind == "remote":   # slim remote + nunchuk with a cable
        R(40, 20, 110, 290, 30)
        C(75, 75, 16)
        C(75, 160, 9, True)
        C(75, 195, 9, True)
        d.ellipse((ox + 170 * u, oy + 120 * u, ox + 270 * u, oy + 290 * u), outline=CYAN, width=lw)
        C(220, 165, 20)
        L((110, 270), (150, 280), (190, 230))
    elif kind == "kbm":    # keyboard + mouse
        R(10, 120, 200, 250, 16)
        for r in range(3):
            for c in range(5):
                C(40 + c * 33, 152 + r * 33, 6, True)
        R(220, 90, 295, 250, 40)
        L((257, 90), (257, 150))
    elif kind == "pad":    # twin-stick controller
        d.rounded_rectangle((ox + 10 * u, oy + 90 * u, ox + 290 * u, oy + 230 * u), radius=70 * u, outline=CYAN,
                            width=lw)
        C(85, 150, 26)
        C(215, 150, 26)
        L((150, 115), (150, 125))
    elif kind == "gyro":   # controller with rotation arcs
        d.rounded_rectangle((ox + 50 * u, oy + 120 * u, ox + 250 * u, oy + 220 * u), radius=50 * u, outline=CYAN,
                            width=lw)
        C(105, 170, 16)
        C(195, 170, 16)
        d.arc((ox + 10 * u, oy + 20 * u, ox + 290 * u, oy + 300 * u), 205, 335, fill=CYAN, width=lw)
        L((262, 52), (282, 88), (240, 92))
    f = ImageFont.truetype(r"C:\Windows\Fonts\bahnschrift.ttf", 118 * S)
    try:
        f.set_variation_by_name("Bold SemiCondensed")
    except Exception:
        pass
    lines = text.split("\n")
    ty = 210 * S - (len(lines) * 130 * S) // 2
    for ln in lines:
        d.text((420 * S, ty), ln, font=f, fill=WHITE)
        ty += 130 * S
    small = img.resize((1500, 420), Image.LANCZOS)
    glow = small.split()[3].filter(ImageFilter.GaussianBlur(30))
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    x, y = 140, H - 420 - 190
    g = Image.new("RGBA", (1500, 420), (40, 200, 255, 0))
    g.putalpha(glow.point(lambda v: v * 0.35))
    out.alpha_composite(g, (x, y))
    out.alpha_composite(small, (x, y))
    return out


if __name__ == "__main__":
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for key, text in {"remote": "WII REMOTE\n+ NUNCHUK", "kbm": "MOUSE &\nKEYBOARD", "pad": "TWIN-STICK\nCONTROLLER",
                      "gyro": "GYRO\nAIMING"}.items():
        plate_icon(key, text).save(out / f"badge_{key}.png")
        print("wrote", key)
