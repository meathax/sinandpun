#!/usr/bin/env python3
"""Full-stage shader-cache warm runs: headless, muted, trailer bot with god mode, one stage per run.

Each run boots build/native/sinandpun.exe --headless into SIN_AUTOSTAGE=<n> with the trailer bot
(SIN_TRAILER_BOT=1, SIN_TRAILER_BOT_GOD=1) and plays until the loaded stage changes away from <n>
(the stage was cleared), the pipeline cache stops growing for --idle seconds, or --timeout passes.
All runs share one data folder (default enhancements ON, audio muted, solo co-op) so aurora records
every pipeline config into <data>/cache/pipeline_cache.db. --merge then adds every current-version
row that the shipped seed lacks to runtime/assets/pipeline/initial_pipeline_cache.db.

Usage (repo root): python tools/warm_pipeline_full.py [--stages 0 1 2] [--timeout 1800] [--merge]
"""
import argparse
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "native" / "sinandpun.exe"
SEED = ROOT / "runtime" / "assets" / "pipeline" / "initial_pipeline_cache.db"
GAME = ROOT / "orig" / "R2VE01"
STAGE_RE = re.compile(r"\[watch\] t=([0-9.]+) loaded stage (\d+)")
REACHED_RE = re.compile(r"\[autostage\] t=[0-9.]+ stage (\d+) load reached")


def cache_rows(db):
    if not db.exists():
        return 0
    try:
        con = sqlite3.connect(f"file:{db.as_posix()}?mode=ro", uri=True, timeout=5)
        n = con.execute("select count(*) from pipeline_cache").fetchone()[0]
        con.close()
        return n
    except sqlite3.Error:
        return -1


def write_config(data):
    data.mkdir(parents=True, exist_ok=True)
    cfg = data / "Config.toml"
    if cfg.exists():
        return
    # Only the keys the run needs; everything else (all graphics enhancements) keeps its default ON value.
    cfg.write_text(
        "[audio]\nmuted = true\nvolume = 0.0\n\n"
        "[controller]\nenhanced_coop = false\n\n"
        f"[paths]\ndvd_root = \"{GAME.as_posix()}\"\n", encoding="utf-8")


def run_stage(stage, data, logs, timeout, idle):
    env = dict(os.environ)
    env.update({
        "SIN_AUTOSTAGE": str(stage), "SIN_TRAILER_BOT": "1", "SIN_TRAILER_BOT_GOD": "1",
        "SIN_TRAILER_BOT_SEED": str(7 + stage), "SIN_VIRTUAL_REMOTE": "1", "SIN_DEBUG_WATCH": "1",
        "SIN_DEBUG_WATCH_HZ": "4",
    })
    db = data / "cache" / "pipeline_cache.db"
    log = logs / f"warm_s{stage}.log"
    start_rows = cache_rows(db)
    state = {"loaded": False, "left": None, "last": None}

    with open(log, "w", encoding="utf-8", errors="replace") as out:
        proc = subprocess.Popen([str(EXE), "--headless", f"--data={data}", f"--game={GAME}"], cwd=EXE.parent, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")

        def pump():
            for line in proc.stdout:
                out.write(line)
                r = REACHED_RE.search(line)
                if r and int(r.group(1)) == stage:
                    state["loaded"] = True  # the boot-time stage byte reads 0 before any stage loads
                m = STAGE_RE.search(line)
                if m:
                    s = int(m.group(2))
                    state["last"] = s
                    if s != stage and state["loaded"] and state["left"] is None:
                        state["left"] = (float(m.group(1)), s)

        reader = threading.Thread(target=pump, daemon=True)
        reader.start()
        t0 = time.time()
        rows, grown = start_rows, t0
        reason = "timeout"
        while time.time() - t0 < timeout:
            time.sleep(5)
            if proc.poll() is not None:
                reason = f"exited {proc.returncode}"
                break
            if state["left"]:
                reason = f"stage left at t={state['left'][0]:.0f}s -> {state['left'][1]}"
                time.sleep(10)  # let the result/next scene draw once
                break
            now_rows = cache_rows(db)
            if now_rows > rows:
                rows, grown = now_rows, time.time()
            if state["loaded"] and time.time() - grown > idle:
                reason = f"no new pipelines for {idle}s"
                break
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(20)
            except subprocess.TimeoutExpired:
                proc.kill()
        reader.join(5)
    end_rows = cache_rows(db)
    print(f"stage {stage}: {reason}; {time.time() - t0:.0f}s; cache rows {start_rows} -> {end_rows}", flush=True)
    return end_rows


def merge(data_dirs):
    con = sqlite3.connect(SEED)
    versions = set(con.execute("select distinct type, config_version from pipeline_cache").fetchall())
    rows = []
    for data in data_dirs:
        src = Path(data).resolve() / "cache" / "pipeline_cache.db"
        s = sqlite3.connect(f"file:{src.as_posix()}?mode=ro", uri=True)
        rows += [r for r in s.execute(
            "select type, hash, config_version, config_size, config, first_frame_used from pipeline_cache")
            if (r[0], r[2]) in versions]
        s.close()
    before = con.execute("select count(*) from pipeline_cache").fetchone()[0]
    con.executemany("insert or ignore into pipeline_cache values (?,?,?,?,?,?)", rows)
    con.commit()
    after = con.execute("select count(*) from pipeline_cache").fetchone()[0]
    con.execute("VACUUM")
    con.close()
    for copy in (ROOT / "build" / "native" / "initial_pipeline_cache.db", ROOT / "ready" / "initial_pipeline_cache.db"):
        if copy.parent.is_dir():
            shutil.copyfile(SEED, copy)
    print(f"seed rows {before} -> {after} (+{after - before})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stages", type=int, nargs="*", default=list(range(8)))
    ap.add_argument("--timeout", type=int, default=1800, help="seconds per stage")
    ap.add_argument("--idle", type=int, default=600, help="stop a stage after this long without new pipelines")
    ap.add_argument("--data", default=str(ROOT / "build" / "warm_full" / "data"))
    ap.add_argument("--merge", action="store_true")
    ap.add_argument("--merge-only", nargs="+", metavar="DATA", help="only merge these data folders into the seed")
    a = ap.parse_args()
    if a.merge_only:
        merge(a.merge_only)
        return 0
    data = Path(a.data).resolve()
    logs = data.parent / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    write_config(data)
    for stage in a.stages:
        run_stage(stage, data, logs, a.timeout, a.idle)
    if a.merge:
        merge([data])
    return 0


if __name__ == "__main__":
    sys.exit(main())
