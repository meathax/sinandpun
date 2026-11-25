"""Writes build/native/UserData/Config.toml for one trailer capture profile.

Usage: python tools/trailer_config.py PROFILE [key=value ...]
Profiles (all set [video] keys; any key=value after the profile overrides one):
  max       every enhancement on, 4x internal resolution, 4x MSAA, 16x AF, HD world + HUD, original 60 Hz present
  original  Wii-like baseline: 1x internal resolution, no MSAA/AF, no enhancements, original HUD and textures
  nofx      max with the Enhancements page off (bounce lighting, materials, combat light, atmosphere)
  notex     max with the HD world texture pack off
The untouched config is kept once as Config.toml.pre-capture.bak; `restore` puts it back.
"""
import re
import shutil
import sys
from pathlib import Path

CFG = Path(__file__).resolve().parent.parent / "build/native/UserData/Config.toml"
BAK = CFG.with_name("Config.toml.pre-capture.bak")

ENH = ["enhancements_enabled", "sharpening_enabled", "ambient_occlusion", "global_illumination", "ray_traced_gi",
       "retain_culled_gi", "strong_bounce", "projected_caustics", "surface_highlights", "probe_materials",
       "combat_light", "contact_shadow", "stage_atmosphere", "per_pixel_lighting", "scene_glow", "enhanced_bloom"]

MAX = {
    "resolution_multiplier": "4", "msaa": "4", "anisotropy": "16", "upscaler": '"off"', "widescreen": "true",
    "frame_rate": '"original"', "vsync": "false", "show_fps": "false", "hd_hud": "true", "hd_world": "true",
    "generate_mips": "true", "texture_replacements": "false", "texture_dumps": "false",
    "disable_copy_filter": "true", "scaling_filter": '"bilinear"', "skip_unready_pipelines": "false",
    **{k: "true" for k in ENH},
}
PROFILES = {
    "max": MAX,
    "original": {**MAX, "resolution_multiplier": "1", "msaa": "1", "anisotropy": "1", "hd_hud": "false",
                 "hd_world": "false", "generate_mips": "false", "disable_copy_filter": "false",
                 **{k: "false" for k in ENH}},
    "nofx": {**MAX, **{k: "false" for k in ENH}},
    "notex": {**MAX, "hd_world": "false"},
}


def write(values, section="video"):
    text = CFG.read_text(encoding="utf-8")
    if f"[{section}]\n" not in text:
        text = text.rstrip("\n") + f"\n\n[{section}]\n"
    head, sep, rest = text.partition(f"[{section}]\n")
    body, nxt, tail = rest.partition("\n[")
    lines = body.splitlines()
    for k, v in values.items():
        pat = re.compile(rf"^{re.escape(k)}\s*=")
        for i, line in enumerate(lines):
            if pat.match(line):
                lines[i] = f"{k} = {v}"
                break
        else:
            lines.append(f"{k} = {v}")
    data = (head + sep + "\n".join(lines) + "\n" + nxt + tail).encode("utf-8")
    tmp = CFG.with_suffix(".tmp")
    tmp.write_bytes(data)
    tmp.replace(CFG)


def main():
    if sys.argv[1] == "restore":
        if BAK.exists():
            shutil.copyfile(BAK, CFG)
        return
    if not BAK.exists():
        shutil.copyfile(CFG, BAK)
    values = dict(PROFILES[sys.argv[1]])
    for kv in sys.argv[2:]:
        k, v = kv.split("=", 1)
        values[k] = v
    write(values)
    # The trailer bot drives both seats; it resolves the player actors through Enhanced co-op.
    write({"enhanced_coop": "true"}, "controller")


if __name__ == "__main__":
    main()
