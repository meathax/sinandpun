"""Decodes the SIN_TRAILER_TAG frame code from a capture.

Usage: python tools/trailer_tags.py CAPTURE.mkv [OUT.json]
Writes a JSON list: capture frame index -> bot frame tag (0 = no tag / gameplay not started, -1 = unreadable).
The tag is 18 blocks of 8x8 px on the bottom row band: a white start block, a black gap, then 16 bits MSB first.
"""
import json
import subprocess
import sys

import numpy as np

FFMPEG = r"D:\Arcade\AI\ffmpeg-8.1.2-full_build\bin\ffmpeg.exe"


def decode(path):
    probe = subprocess.run([FFMPEG, "-hide_banner", "-i", path], capture_output=True, text=True).stderr
    h = 2160
    for tok in probe.split():
        if "x" in tok and tok.rstrip(",").split("x")[0].isdigit() and tok.rstrip(",").split("x")[-1].isdigit():
            w0, h0 = tok.rstrip(",").split("x")
            if int(w0) >= 640:
                h = int(h0)
                break
    cmd = [FFMPEG, "-hide_banner", "-loglevel", "error", "-i", path, "-fps_mode", "passthrough", "-vf", f"crop=144:8:0:{h - 8},format=gray",
           "-f", "rawvideo", "-"]
    raw = subprocess.run(cmd, capture_output=True).stdout
    frames = np.frombuffer(raw, np.uint8).reshape(-1, 8, 144)
    centres = frames[:, 2:6, :].reshape(len(frames), 4, 18, 8)[:, :, :, 2:6].mean(axis=(1, 3))
    tags = []
    for c in centres:
        if c[0] < 160 or c[1] > 90:
            tags.append(-1)
            continue
        bits = c[2:] > 128
        if np.any((c[2:] > 90) & (c[2:] < 160)):
            tags.append(-1)
            continue
        v = 0
        for b in bits:
            v = (v << 1) | int(b)
        tags.append(v)
    return tags


if __name__ == "__main__":
    tags = decode(sys.argv[1])
    if len(sys.argv) > 2:
        json.dump(tags, open(sys.argv[2], "w"))
    valid = [t for t in tags if t > 0]
    print(f"{len(tags)} frames, {len(valid)} tagged, unreadable {tags.count(-1)}, "
          f"range {min(valid) if valid else '-'}..{max(valid) if valid else '-'}, "
          f"unique {len(set(valid))}, dup {len(valid) - len(set(valid))}")
