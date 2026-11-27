"""Benchmark harness: boot time, stage load time, FPS/frametime with uncapped presentation and all enhancements.

Usage (repo root):
  python tools/perf_bench.py --stage 2 --seconds 90 --name base_s2 [--exe build/native/sinandpun.exe] [--env K=V ...]

* Takes the shared build/run lock (docs/PERF-AGENT-GUIDE.md), runs with --data=build/perf/data-<name> seeded from
  build/native/UserData (NAND, caches unless --cold) and a Config.toml patched with the benchmark video settings (frame_rate uncapped, vsync off, every enhancement on), runs the game with SIN_PERF_LOG=1,
  SIN_AUTOSTAGE=<stage> and a looping A-press remote script, then releases the lock.
* Every stderr line is stamped with wall time since process start, so boot/load milestones need no runtime timer.
* Writes build/perf/<name>.log (stamped) and build/perf/<name>.json (summary) and prints the summary.
"""
import argparse, json, os, re, shutil, statistics, subprocess, sys, threading, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCK = ROOT / "build" / "agent.lock"
SCRIPT = ("32|0.3:A|10|0.21::-1/0|0.5|0.3:A|5|0.15::-1/0|0.3|0.19::0/-1|0.5|0.3:A|1.5|0.056::1/0|0.3|0.23::0/1|0.5|"
          "0.3:A|4|0.3:PLUS|4|0.3:A|4|0.3:A|4|0.04::-1/0|0.2|0.095::0/-1|0.5|0.3:A|4|0.3:A|4|0.3:A|4|0.3:A|120")

VIDEO = {
    "frame_rate": '"uncapped"', "vsync": "false", "vrr": '"off"', "vrr_limiter": "false", "frame_rate_cap": "1000",
    "show_fps": "false", "upscaler": '"off"', "dlss_frame_generation": "false",
    "enhancements_enabled": "true", "ambient_occlusion": "true", "global_illumination": "true",
    "ray_traced_gi": "true", "retain_culled_gi": "true", "strong_bounce": "true", "projected_caustics": "true",
    "surface_highlights": "true", "probe_materials": "true", "combat_light": "true", "contact_shadow": "true",
    "stage_atmosphere": "true", "hd_hud": "true", "hd_world": "true", "scene_glow": "true",
    "enhanced_bloom": "true", "generate_mips": "true", "per_pixel_lighting": "true", "sharpening_enabled": "true",
}


def take_lock(who):
    for _ in range(1200):
        try:
            LOCK.mkdir()
            (LOCK / "owner.txt").write_text(f"{who} {time.strftime('%H:%M:%S')}", encoding="utf-8")
            return
        except FileExistsError:
            time.sleep(1)
    raise SystemExit("could not get build lock")


def patch_config(text):
    lines = text.splitlines()
    out, in_video, seen = [], False, set()
    for ln in lines:
        s = ln.strip()
        if s.startswith("["):
            if in_video:
                out += [f"{k} = {v}" for k, v in VIDEO.items() if k not in seen]
            in_video = s == "[video]"
        elif in_video and "=" in s:
            k = s.split("=", 1)[0].strip()
            if k in VIDEO:
                out.append(f"{k} = {VIDEO[k]}")
                seen.add(k)
                continue
        out.append(ln)
    if in_video:
        out += [f"{k} = {v}" for k, v in VIDEO.items() if k not in seen]
    return "\n".join(out) + "\n"


def force_foreground(pid):
    """Bring the game's window to the front: a background window is DWM-composed, which vblank-locks presents."""
    import ctypes
    from ctypes import wintypes
    u = ctypes.windll.user32
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, _):
        p = wintypes.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u.IsWindowVisible(h):
            found.append(h)
        return True
    u.EnumWindows(cb, 0)
    for h in found:
        if u.GetForegroundWindow() == h:
            return True
        u.keybd_event(0x12, 0, 0, 0)  # Alt down/up lifts the foreground lock
        u.keybd_event(0x12, 0, 2, 0)
        u.ShowWindow(h, 9)
        u.SetForegroundWindow(h)
        return u.GetForegroundWindow() == h
    return False


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))] if v else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", type=int, default=2)
    ap.add_argument("--seconds", type=int, default=90)
    ap.add_argument("--name", required=True)
    ap.add_argument("--exe", default="build/native/sinandpun.exe")
    ap.add_argument("--env", nargs="*", default=[])
    ap.add_argument("--no-stage", action="store_true", help="boot to title only (no autostage)")
    ap.add_argument("--cold", action="store_true", help="do not seed the shader/pipeline caches")
    ap.add_argument("--video", nargs="*", default=[], help="extra [video] overrides key=value (TOML literal)")
    a = ap.parse_args()
    for kv in a.video:
        k, v = kv.split("=", 1)
        VIDEO[k] = v

    out_dir = ROOT / "build" / "perf"
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = (ROOT / a.exe).resolve()
    src = ROOT / "build" / "native" / "UserData"
    data = out_dir / f"data-{a.name}"
    shutil.rmtree(data, ignore_errors=True)
    data.mkdir()
    shutil.copytree(src / "NAND", data / "NAND")
    if not a.cold:
        shutil.copytree(src / "Cache", data / "Cache")
    (data / "Config.toml").write_text(patch_config((src / "Config.toml").read_text(encoding="utf-8-sig")), encoding="utf-8")
    script = data / "input.txt"
    # A at a fixed pointer spot: passes the title/menus, then fires continuously in the stage.
    script.write_text("0.2:A:@0.113/0.72|2.8::@0.113/0.72|loop", encoding="utf-8")
    env = dict(os.environ, SIN_PERF_LOG="1", SIN_PERF_SPIKES="2", SIN_VIRTUAL_REMOTE="1", SIN_VREMOTE_SCRIPT="1",
               SIN_VREMOTE_SCRIPT_FILE=str(script))
    if not a.no_stage:
        env["SIN_AUTOSTAGE"] = str(a.stage)
    for kv in a.env:
        k, v = kv.split("=", 1)
        env[k] = v

    take_lock(f"perf_bench {a.name}")
    lines = []
    try:
        t0 = time.perf_counter()
        p = subprocess.Popen([str(exe), "--game=" + str(ROOT / "orig" / "R2VE01"), "--data=" + str(data)], cwd=str(ROOT / "build" / "native"), env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

        def reader():
            for raw in p.stderr:
                lines.append((time.perf_counter() - t0, raw.decode("utf-8", "replace").rstrip()))
        th = threading.Thread(target=reader, daemon=True)
        th.start()
        next_fg, fg_ok = 2.0, []
        while time.perf_counter() - t0 < a.seconds and p.poll() is None:
            time.sleep(0.5)
            if time.perf_counter() - t0 >= next_fg:
                fg_ok.append(force_foreground(p.pid))
                next_fg += 5.0
        subprocess.run(["taskkill", "/pid", str(p.pid)], capture_output=True)
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        th.join(3)
    finally:
        shutil.rmtree(LOCK, ignore_errors=True)

    shutil.rmtree(data / "Cache", ignore_errors=True)
    (out_dir / f"{a.name}.log").write_text("\n".join(f"{t:8.3f} {s}" for t, s in lines), encoding="utf-8")
    summ = summarize(lines)
    summ["name"], summ["stage"], summ["exe"] = a.name, a.stage, str(exe)
    summ["foreground_checks"] = f"{sum(fg_ok)}/{len(fg_ok)}"
    (out_dir / f"{a.name}.json").write_text(json.dumps(summ, indent=1), encoding="utf-8")
    print(json.dumps(summ, indent=1))


def summarize(lines):
    s = {}
    num = lambda pat, txt: float(re.search(pat, txt).group(1)) if re.search(pat, txt) else None
    first = lambda pat: next((t for t, x in lines if re.search(pat, x)), None)
    s["t_first_line"] = lines[0][0] if lines else None
    s["t_stage_loaded"] = first(r"\[autostage\].*F state 3 -> 4")
    s["t_autostage_load_reached"] = s["t_stage_loaded"]
    guest, present, cpu, gfp99, gfmax, pp50, pp99, pmax = [], [], [], [], [], [], [], []
    after = s["t_autostage_load_reached"] or 0.0
    for t, x in lines:
        if t < after + 15:  # steady gameplay only
            continue
        if x.startswith("[runtime] [perf] fps=") or "[perf] fps=" in x:
            guest.append(num(r"guest=([\d.]+)", x)); present.append(num(r"present=([\d.]+)", x))
            cpu.append(num(r"cpu=([\d.]+)%", x))
        elif "[perf-present]" in x:
            pp50.append(num(r"p50=([\d.]+)ms", x)); pp99.append(num(r"p99=([\d.]+)ms", x))
            pmax.append(num(r"max=([\d.]+)ms", x))
        elif "[perf-gf]" in x:
            gfp99.append(num(r"wall_ms p50=[\d.]+ p99=([\d.]+)", x)); gfmax.append(num(r"wall_ms .*?max=([\d.]+)", x))
    st = lambda v: {"n": len(v), "avg": round(statistics.mean(v), 2), "min": round(min(v), 2),
                    "p1": round(pct(v, 0.01), 2), "med": round(statistics.median(v), 2)} if v else None
    # Per guest frame (SIN_PERF_SPIKES=2): busy = interval minus the game's own retrace wait and our pace
    # wait, i.e. the time the frame really needed (headroom against the 16.7 ms budget).
    busy, endf = [], []
    for t, x in lines:
        if t < after + 15 or "[spike]" not in x:
            continue
        iv, vw, pc, ef = (num(r + r"=(-?[\d.]+)", x) for r in ("interval", "viwait", "pace", "endframe"))
        if iv is None or vw is None:
            continue
        busy.append(iv - vw - (pc or 0.0))
        endf.append(ef or 0.0)
    s["frames"] = len(busy)
    s["busy_ms"] = {"p50": round(pct(busy, .5), 2), "p90": round(pct(busy, .9), 2), "p99": round(pct(busy, .99), 2),
                    "p999": round(pct(busy, .999), 2), "over16_7": sum(1 for b in busy if b > 16.7)} if busy else None
    s["endframe_ms"] = {"p50": round(pct(endf, .5), 2), "p99": round(pct(endf, .99), 2)} if endf else None
    s["present_fps"] = st(present)
    s["guest_fps"] = st(guest)
    s["guest_below_59"] = sum(1 for g in guest if g < 59)
    s["cpu_pct"] = st(cpu)
    s["present_p50_ms_med"] = round(statistics.median(pp50), 3) if pp50 else None
    s["present_p99_ms_med"] = round(statistics.median(pp99), 3) if pp99 else None
    s["present_max_ms_max"] = round(max(pmax), 2) if pmax else None
    s["guest_wall_p99_med"] = round(statistics.median(gfp99), 2) if gfp99 else None
    s["guest_wall_max"] = round(max(gfmax), 2) if gfmax else None
    return s


if __name__ == "__main__":
    main()
