"""Assembles the trailer from tagged captures (tools/trailer_capture.ps1 with SIN_TRAILER_TAG).

Each capture carries a 16-bit bot-frame code in its bottom-left 144x8 px. Two runs of the frame-locked
bot with different graphics settings replay the same inputs, so frames with equal tags show the same
moment. Shots are addressed by tag; the code strip is painted out of every output frame.

Usage: python tools/trailer_edit.py EDL.py OUT.mp4 [--preview N] [--only SHOT_INDEX]
The EDL module defines SHOTS (list of dicts, see make_frame) and optional TITLES dir.
"""
import importlib.util
import math
import subprocess
import sys
from pathlib import Path

import cv2
import numpy as np

FFMPEG = r"D:\Arcade\AI\ffmpeg-8.1.2-full_build\bin\ffmpeg.exe"
W, H = 3840, 2160
FS = W * H * 3
RAW = Path(__file__).resolve().parent.parent / "build/trailer/raw"


def read_tag(f):
    strip = cv2.cvtColor(f[H - 8:, :144], cv2.COLOR_BGR2GRAY).astype(np.float32)
    c = strip[2:6].reshape(4, 18, 8)[:, :, 2:6].mean(axis=(0, 2))
    if c[0] < 160 or c[1] > 90 or np.any((c[2:] > 90) & (c[2:] < 160)):
        return -1
    v = 0
    for b in c[2:] > 128:
        v = (v << 1) | int(b)
    return v


def scrub_tag(f):
    f[H - 8:, :152] = f[H - 9:H - 8, :152]
    return f


class Reader:
    """Sequential reader returning the frame for a requested (monotonic) tag."""

    def __init__(self, name, first_tag):
        self.path = RAW / f"{name}.mkv"
        import json
        tags = json.load(open(RAW / f"{name}.tags.json"))
        idx = next(i for i, t in enumerate(tags) if t >= first_tag)
        # Exact seek: the capture is variable-rate with B-frames, so sort packet times into display order
        # (cached beside the capture) and start ffmpeg at the wanted frame's timestamp.
        pts_file = RAW / f"{name}.pts.json"
        if pts_file.exists():
            pts = json.load(open(pts_file))
        else:
            out = subprocess.run([FFMPEG.replace("ffmpeg.exe", "ffprobe.exe"), "-v", "error", "-select_streams", "v",
                                  "-show_entries", "packet=pts_time", "-of", "csv=p=0", str(self.path)],
                                 capture_output=True, text=True).stdout
            pts = sorted(float(x) for x in out.split() if x.strip())
            json.dump(pts, open(pts_file, "w"))
        idx = max(0, idx - 2)
        seek = max(0.0, pts[min(idx, len(pts) - 1)] - 0.0005)
        self.proc = subprocess.Popen(
            [FFMPEG, "-hide_banner", "-loglevel", "error", "-hwaccel", "cuda", "-ss", f"{seek:.3f}",
             "-i", str(self.path), "-fps_mode", "passthrough",
             "-f", "rawvideo", "-pix_fmt", "bgr24", "-"],
            stdout=subprocess.PIPE, bufsize=FS * 2)
        self.cur, self.cur_tag, self.prev = None, -1, None
        self.name = name

    def _next(self):
        buf = self.proc.stdout.read(FS)
        if len(buf) < FS:
            raise EOFError(f"{self.name}: ran out of frames at tag {self.cur_tag}")
        f = np.frombuffer(buf, np.uint8).reshape(H, W, 3).copy()
        t = read_tag(f)
        if t < 0 and self.cur is not None:
            t = self.cur_tag  # unreadable code (rare): treat as a repeat
        self.prev, self.cur, self.cur_tag = self.cur, f, t

    def at(self, tag):
        while self.cur is None or self.cur_tag < tag:
            self._next()
        return self.cur

    def close(self):
        self.proc.kill()


def ease(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3 - 2 * x)


def ease_io(x):
    x = min(max(x, 0.0), 1.0)
    return 0.5 - 0.5 * math.cos(math.pi * x)


def zoom(f, z, cx=0.5, cy=0.5):
    if z <= 1.0001:
        return f
    cw, ch = W / z, H / z
    x0 = min(max(cx * W - cw / 2, 0), W - cw)
    y0 = min(max(cy * H - ch / 2, 0), H - ch)
    m = np.float32([[z, 0, -x0 * z], [0, z, -y0 * z]])
    return cv2.warpAffine(f, m, (W, H), flags=cv2.INTER_LANCZOS4)


def wipe(before, after, p, angle=12.0):
    """Reveal `after` left-to-right with a slanted line at progress p (0..1). Returns frame."""
    if p <= 0:
        return before
    if p >= 1:
        return after
    slope = math.tan(math.radians(angle))
    span = W + H * slope + 240
    x = -120 - H * slope / 2 + p * span
    ys = np.arange(H, dtype=np.float32)[:, None]
    xs = np.arange(W, dtype=np.float32)[None, :]
    edge = x + (H / 2 - ys) * slope  # line x per row
    d = xs - edge                     # <0: revealed (after) side
    mask = np.clip(0.5 - d / 2.0, 0, 1)[..., None]
    out = after.astype(np.float32) * mask + before.astype(np.float32) * (1 - mask)
    # glowing seam: thin white core plus soft cyan bloom
    core = np.exp(-(d / 3.2) ** 2)[..., None]
    bloom = np.exp(-(d / 38.0) ** 2)[..., None]
    out = out + bloom * np.float32([255, 210, 60]) * 0.45 + core * np.float32([255, 255, 255]) * 0.95
    return np.clip(out, 0, 255).astype(np.uint8)


def over(f, rgba, alpha=1.0, dx=0):
    if rgba is None or alpha <= 0:
        return f
    if dx:
        rgba = np.roll(rgba, dx, axis=1)
        if dx > 0:
            rgba[:, :dx] = 0
    a = rgba[..., 3:4].astype(np.float32) / 255 * alpha
    out = f.astype(np.float32) * (1 - a) + rgba[..., :3].astype(np.float32) * a
    return out.astype(np.uint8)


_png = {}


def png(path):
    if path not in _png:
        im = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
        _png[path] = im  # BGRA
    return _png[path]


def label(f, text, corner, alpha):
    if alpha <= 0:
        return f
    font = cv2.FONT_HERSHEY_DUPLEX
    s, th = 2.3, 4
    (tw, tht), _ = cv2.getTextSize(text, font, s, th)
    pad = 34
    x = 90 if corner == "l" else W - 90 - tw - 2 * pad
    y = H - 170
    ov = f.copy()
    cv2.rectangle(ov, (x, y - tht - pad), (x + tw + 2 * pad, y + pad), (20, 12, 8), -1)
    accent = (40, 60, 230) if corner == "l" else (255, 220, 40)
    cv2.rectangle(ov, (x, y - tht - pad), (x + 10, y + pad), accent, -1)
    cv2.putText(ov, text, (x + pad + 6, y), font, s, (245, 245, 245), th, cv2.LINE_AA)
    return cv2.addWeighted(ov, alpha * 0.92, f, 1 - alpha * 0.92, 0)


def render(edl, out, preview=0, only=None):
    shots = edl.SHOTS
    total = sum(s["len"] for s in shots)
    print(f"{len(shots)} shots, {total} frames = {total / 60:.2f}s")
    if preview:
        enc = [FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "bgr24", "-s",
               f"{W}x{H}", "-r", "60", "-i", "-", "-vf", f"scale={W // preview}:-2", "-c:v", "h264_nvenc",
               "-preset", "p4", "-cq", "24", "-pix_fmt", "yuv420p", out]
    else:
        enc = [FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "bgr24", "-s",
               f"{W}x{H}", "-r", "60", "-i", "-", "-c:v", "h264_nvenc", "-preset", "p7", "-tune", "hq",
               "-profile:v", "high", "-level", "5.2", "-rc", "vbr", "-cq", "15", "-b:v", "110M", "-maxrate", "160M",
               "-bufsize", "220M", "-spatial-aq", "1", "-temporal-aq", "1", "-pix_fmt", "yuv420p",
               "-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709", "-movflags", "+faststart",
               out]
    sink = subprocess.Popen(enc, stdin=subprocess.PIPE)
    gframe = 0
    prev_last = None
    for si, s in enumerate(shots):
        if only is not None and si != only:
            gframe += s["len"]
            continue
        readers = {}
        for key in ("before", "after", "src"):
            if key in s:
                readers[key] = Reader(s[key], s["tag"])
        title = png(edl.TITLES / f"title_{s['title']}.png") if s.get("title") else None
        for i in range(s["len"]):
            t = s["tag"] + int(i * s.get("speed", 1.0))
            z = s.get("zoom", 1.0)
            if "zoom_from" in s:
                z = s["zoom_from"] + (z - s["zoom_from"]) * ease_io(i / max(1, s.get("zoom_frames", s["len"])))
            cx, cy = s.get("cx", 0.5), s.get("cy", 0.5)
            if "src" in readers:
                f = zoom(scrub_tag(readers["src"].at(t).copy()), z, cx, cy)
            else:
                p = ease_io((i - s["wipe_at"]) / s["wipe_len"])
                a = zoom(scrub_tag(readers["after"].at(t).copy()), z, cx, cy)
                # once revealed, the before clip is no longer read (it may end before the shot does)
                b = a if p >= 1 else zoom(scrub_tag(readers["before"].at(t).copy()), z, cx, cy)
                f = wipe(b, a, p)
                lab = min(1.0, i / 12) * (1 - ease((i - (s["wipe_at"] + s["wipe_len"] + 20)) / 15))
                f = label(f, s.get("before_label", "ORIGINAL"), "l", lab * (1 - ease((p - 0.75) / 0.25)))
                f = label(f, s.get("after_label", "RECOMPILED"), "r", lab * ease(p / 0.25))
            # cut in: brief flash from white for the first frames of a shot (except the very first)
            if s.get("flash", si > 0) and i < 8:
                k = (1 - i / 8) ** 2 * 0.55
                f = cv2.addWeighted(f, 1 - k, np.full_like(f, 255), k, 0)
            if title is not None:
                ti, to = s.get("title_in", 0), s.get("title_out", s["len"])
                a_in = 1.0 if ti < 0 else ease((i - ti) / 18)
                a_out = 1 - ease((i - (to - 18)) / 18)
                slide = 0 if ti < 0 else int((1 - ease((i - ti) / 22)) * -160)
                f = over(f, title, min(a_in, a_out), dx=slide)
            if s.get("badge"):
                bi, bo = s.get("badge_in", 0), s.get("badge_out", s["len"])
                ba = min(ease((i - bi) / 14), 1 - ease((i - (bo - 14)) / 14))
                f = over(f, png(edl.TITLES / f"badge_{s['badge']}.png"), ba, dx=int((1 - ease((i - bi) / 18)) * -120))
            fade_in = s.get("fade_in", 0)
            if fade_in and i < fade_in:
                f = (f.astype(np.float32) * ease(i / fade_in)).astype(np.uint8)
            fade_out = s.get("fade_out", 0)
            if fade_out and i >= s["len"] - fade_out:
                f = (f.astype(np.float32) * (1 - ease((i - (s["len"] - fade_out)) / fade_out))).astype(np.uint8)
            sink.stdin.write(f.tobytes())
            gframe += 1
            if gframe % 60 == 0:
                print(f"  {gframe}/{total}", flush=True)
        for r in readers.values():
            r.close()
    sink.stdin.close()
    sink.wait()
    print("done", out)


if __name__ == "__main__":
    spec = importlib.util.spec_from_file_location("edl", sys.argv[1])
    edl = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(edl)
    preview = int(sys.argv[sys.argv.index("--preview") + 1]) if "--preview" in sys.argv else 0
    only = int(sys.argv[sys.argv.index("--only") + 1]) if "--only" in sys.argv else None
    render(edl, sys.argv[2], preview, only)
