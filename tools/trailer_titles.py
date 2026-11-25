"""Renders trailer heading cards as 3840x2160 RGBA PNGs (original typography; no game logo art).

Style: heavy condensed italic caps with a chrome gradient, dark outline and a cyan energy glow, sat on an
angled red-orange slash with a small tracked subtitle - echoing the game's chrome-and-cyan HUD look.
Usage: python tools/trailer_titles.py OUTDIR
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 3840, 2160
FONT = r"C:\Windows\Fonts\bahnschrift.ttf"

CARDS = {
    "res": ("4K  &  UNCAPPED", "HIGH-RESOLUTION RENDERING  ·  UNLOCKED FRAME RATE"),
    "fx": ("RAY-TRACED LIGHTING", "BOUNCE LIGHT  ·  REFLECTIONS  ·  COMBAT LIGHTING  ·  ATMOSPHERE"),
    "tex": ("HD TEXTURE PACK", "292 REMASTERED WORLD & CHARACTER TEXTURES"),
    "ctl": ("PLAY YOUR WAY", "WII REMOTE  ·  MOUSE & KEYBOARD  ·  CONTROLLER  ·  GYRO AIM"),
    "end": ("SIN & PUNISHMENT", "STAR SUCCESSOR  ·  RECOMPILED"),
}


def font(size, name="Bold Condensed"):
    f = ImageFont.truetype(FONT, size)
    try:
        f.set_variation_by_name(name)
    except Exception:
        pass
    return f


def shear(img, k=0.22):
    w, h = img.size
    pad = int(h * k) + 4
    out = img.transform((w + pad, h), Image.AFFINE, (1, k, -pad * 0 - k * h + pad * 0, 0, 1, 0), Image.BICUBIC)
    return out


def text_mask(text, size, tracking=0, name="Bold Condensed"):
    f = font(size, name)
    widths = [f.getbbox(c)[2] - f.getbbox(c)[0] if c != " " else size * 0.28 for c in text]
    adv = [f.getlength(c) for c in text]
    total = int(sum(adv) + tracking * (len(text) - 1)) + size
    asc, desc = f.getmetrics()
    m = Image.new("L", (total, asc + desc + size // 4), 0)
    d = ImageDraw.Draw(m)
    x = size // 2
    for c, a in zip(text, adv):
        d.text((x, size // 8), c, font=f, fill=255)
        x += a + tracking
    bb = m.getbbox()
    m = m.crop((bb[0], 0, bb[2], m.height))
    return shear(m, 0.2)


def card(title, subtitle, big=230, centre=False):
    layer = np.zeros((H, W, 4), np.float32)
    tm = text_mask(title, big, tracking=int(big * 0.04))
    tw, th = tm.size
    if centre:
        x0, y0 = (W - tw) // 2, int(H * 0.40) - th // 2
    else:
        x0, y0 = 230, int(H * 0.085)
    canvas_m = Image.new("L", (W, H), 0)
    canvas_m.paste(tm, (x0, y0))
    m = np.asarray(canvas_m, np.float32) / 255

    # slash bar under the heading
    bar = Image.new("L", (W, H), 0)
    bd = ImageDraw.Draw(bar)
    by = y0 + th - int(big * 0.06)
    bh = int(big * 0.30)
    bx0 = x0 - 60 if not centre else (W - tw) // 2 - 40
    bx1 = x0 + tw + 140 if not centre else (W + tw) // 2 + 40
    bd.polygon([(bx0 + bh, by), (bx1 + bh, by), (bx1, by + bh), (bx0, by + bh)], fill=255)
    bar_m = np.asarray(bar.filter(ImageFilter.GaussianBlur(1.2)), np.float32) / 255
    # bar gradient: deep red to orange
    gx = np.linspace(0, 1, W, dtype=np.float32)[None, :, None]
    bar_rgb = (1 - gx) * np.array([0.78, 0.05, 0.08], np.float32) + gx * np.array([1.0, 0.55, 0.08], np.float32)
    bar_rgb = np.broadcast_to(bar_rgb, (H, W, 3))

    sm = text_mask(subtitle, int(big * 0.215), tracking=int(big * 0.05), name="SemiBold SemiCondensed")
    sub = Image.new("L", (W, H), 0)
    sx = bx0 + bh + 40 if not centre else (W - sm.size[0]) // 2
    sub.paste(sm, (sx, by + (bh - sm.size[1]) // 2 + 4))
    sub_m = np.asarray(sub, np.float32) / 255

    # outline + glow for the heading
    outline = np.asarray(canvas_m.filter(ImageFilter.MaxFilter(17)).filter(ImageFilter.GaussianBlur(1.5)), np.float32) / 255
    glow = np.asarray(canvas_m.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.GaussianBlur(38)), np.float32) / 255
    shadow = np.asarray(canvas_m.filter(ImageFilter.MaxFilter(21)).filter(ImageFilter.GaussianBlur(22)), np.float32) / 255
    shadow = np.roll(shadow, (18, 22), axis=(0, 1))

    # chrome gradient across heading height
    yy = (np.arange(H, dtype=np.float32)[:, None] - y0) / max(th, 1)
    yy = np.clip(yy, 0, 1)
    chrome = np.where(yy < 0.52, 1.0 - 0.18 * (yy / 0.52), 0.70 + 0.30 * (np.clip(yy - 0.52, 0, 1) / 0.48) ** 0.8)
    band = np.exp(-((yy - 0.53) ** 2) / 0.0012) * 0.35
    lum = np.clip(chrome - band, 0, 1)[..., None]
    chrome_rgb = lum * np.array([0.97, 0.98, 1.0], np.float32) + (1 - lum) * np.array([0.30, 0.42, 0.55], np.float32)

    def over(rgb, a):
        nonlocal layer
        a = a[..., None]
        layer[..., :3] = rgb * a + layer[..., :3] * (1 - a)
        layer[..., 3:] = a + layer[..., 3:] * (1 - a)

    over(np.zeros((H, W, 3), np.float32), shadow * 0.75)
    over(np.broadcast_to(np.array([0.10, 0.85, 1.0], np.float32), (H, W, 3)), np.clip(glow * 1.25, 0, 0.85))
    over(bar_rgb, bar_m * 0.96)
    over(np.broadcast_to(np.array([1, 1, 1], np.float32), (H, W, 3)), sub_m)
    over(np.broadcast_to(np.array([0.02, 0.04, 0.10], np.float32), (H, W, 3)), outline)
    over(chrome_rgb, m)
    return Image.fromarray((np.clip(layer, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA")


if __name__ == "__main__":
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for key, (t, s) in CARDS.items():
        card(t, s, big=380 if key == "end" else 330, centre=key == "end").save(out / f"title_{key}.png")
        print("wrote", key)
