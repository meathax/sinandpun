#!/usr/bin/env python3
"""Deterministic artifact/log checks for Sin save, HOME/quit and SDL audio acceptance."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path
from typing import Any


SLOT_PATTERN = re.compile(r"^sdat\d+\.bin$", re.IGNORECASE)
OPEN_RE = re.compile(r'\[nand\]\s+accept ios_open .*?path="([^"]+)"')
READ_RE = re.compile(r'\[nand\]\s+accept ios_read .*?bytes=(\d+).*?path="([^"]+)"')
AUDIO_OPEN_RE = re.compile(
    r"\[audio-dbg\]\s+backend opened sdl=S16LE rate=(\d+)Hz channels=(\d+)"
)
AUDIO_REPORT_RE = re.compile(
    r"\[audio-dbg\]\s+1s:.*?sdlBlocks=\+(\d+).*?"
    r"sdlNonSilentBlocks=\+(\d+).*?sdlPeak=(\d+)"
)
QUIT_RE = re.compile(
    r"(?:OSReturnToMenu|OSRestart|OSShutdownSystem|OSReturnToDataManager)"
    r": guest requested exit, shutting down cleanly"
)
NAND_FAILURE_RE = re.compile(r"\[nand\].*\bERROR\b", re.IGNORECASE)
PHYSICAL_BLOCKER = (
    "No repository-owned physical audio loopback capture/routing tool is configured; "
    "PCM queued to SDL cannot prove signal reaches a DAC, speaker, or human."
)


def fail(message: str) -> None:
    raise ValueError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def inspect_nand(nand_dir: Path, expected_size: int = 1600) -> dict[str, Any]:
    nand_dir = nand_dir.resolve()
    if not nand_dir.is_dir():
        fail(f"NAND data directory does not exist: {nand_dir}")
    shadows = sorted(str(path.relative_to(nand_dir)) for path in nand_dir.rglob("*.nandsafe.tmp"))
    if shadows:
        fail("uncommitted NAND write shadow(s): " + ", ".join(shadows))
    slots = sorted(
        (path for path in nand_dir.iterdir() if path.is_file() and SLOT_PATTERN.match(path.name)),
        key=lambda path: path.name.lower(),
    )
    if not slots:
        fail(f"no sdat slot files found in {nand_dir}")
    entries: dict[str, dict[str, Any]] = {}
    for path in slots:
        size = path.stat().st_size
        if size != expected_size:
            fail(f"{path.name} size is {size}, expected {expected_size}")
        entries[path.name.lower()] = {"size": size, "sha256": sha256(path)}
    return {
        "schema": 1,
        "nand_dir": str(nand_dir),
        "expected_size": expected_size,
        "slots": entries,
    }


def load_manifest(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        manifest = json.load(stream)
    if manifest.get("schema") != 1 or not isinstance(manifest.get("slots"), dict):
        fail(f"invalid NAND manifest: {path}")
    return manifest


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def changed_slots(before: dict[str, Any], after: dict[str, Any]) -> list[str]:
    before_slots = before["slots"]
    after_slots = after["slots"]
    if set(before_slots) != set(after_slots):
        missing = sorted(set(before_slots) - set(after_slots))
        added = sorted(set(after_slots) - set(before_slots))
        fail(f"slot set changed (missing={missing}, added={added})")
    changed = []
    for name, expected in before_slots.items():
        actual = after_slots[name]
        if actual["size"] != expected["size"]:
            fail(f"{name} size changed across acceptance: {expected['size']} -> {actual['size']}")
        if actual["sha256"] != expected["sha256"]:
            changed.append(name)
    return changed


def verify_save(before_path: Path, nand_dir: Path, output_path: Path | None) -> dict[str, Any]:
    before = load_manifest(before_path)
    after = inspect_nand(nand_dir, int(before["expected_size"]))
    changed = changed_slots(before, after)
    if not changed:
        fail("save run did not change any sdat slot hash")
    result = {
        "status": "PASS",
        "stage": "save",
        "changed_slots": changed,
        "slots": after["slots"],
        "nandsafe_tmp": "absent",
    }
    if output_path:
        write_json(output_path, after)
    return result


def read_log(path: Path) -> str:
    if not path.is_file():
        fail(f"log does not exist: {path}")
    return path.read_text(encoding="utf-8", errors="replace")


def verify_runtime_log(log: Path, exit_code: int) -> dict[str, Any]:
    text = read_log(log)
    if exit_code != 0:
        fail(f"game exited with code {exit_code}, expected 0")
    if not QUIT_RE.search(text):
        fail("no clean guest HOME/quit exit marker in stderr")
    if NAND_FAILURE_RE.search(text):
        fail("a [nand] ERROR line was logged")
    opened = AUDIO_OPEN_RE.findall(text)
    if not opened:
        fail("SDL playback stream was not opened (missing [audio-dbg] backend opened)")
    reports = [
        (int(blocks), int(non_silent), int(peak))
        for blocks, non_silent, peak in AUDIO_REPORT_RE.findall(text)
    ]
    accepted = next(
        (item for item in reports if item[0] > 0 and item[1] > 0 and item[2] > 0),
        None,
    )
    if accepted is None:
        fail("SDL accepted no non-silent PCM block (sdlBlocks/sdlNonSilentBlocks/sdlPeak)")
    return {
        "status": "PASS",
        "stage": "home_audio",
        "exit_code": 0,
        "home_exit": "guest marker and clean process exit",
        "sdl_backend": {"rate_hz": int(opened[0][0]), "channels": int(opened[0][1])},
        "sdl_non_silent_pcm": {
            "blocks": accepted[0],
            "non_silent_blocks": accepted[1],
            "peak": accepted[2],
        },
        "human_audibility": {"status": "BLOCKED", "reason": PHYSICAL_BLOCKER},
    }


def verify_reload(
    before_path: Path,
    saved_path: Path,
    nand_dir: Path,
    log_path: Path,
    exit_code: int,
) -> dict[str, Any]:
    before = load_manifest(before_path)
    saved = load_manifest(saved_path)
    saved_changed = changed_slots(before, saved)
    if not saved_changed:
        fail("saved manifest has no progress-bearing slot change")
    after_reload = inspect_nand(nand_dir, int(saved["expected_size"]))
    post_changed = changed_slots(saved, after_reload)
    if post_changed:
        fail("reload unexpectedly modified slot(s): " + ", ".join(post_changed))

    text = read_log(log_path)
    opened = [Path(path.replace("\\", "/")).name.lower() for path in OPEN_RE.findall(text)]
    reads = [
        (int(length), Path(path.replace("\\", "/")).name.lower())
        for length, path in READ_RE.findall(text)
    ]
    expected = {name.lower() for name in saved_changed}
    read_hits = [
        {"slot": name, "bytes": length}
        for length, name in reads
        if name in expected and name in opened and length > 0
    ]
    if not read_hits:
        fail(
            "reload log did not prove a non-empty read of the progress-bearing slot(s): "
            + ", ".join(sorted(expected))
        )
    runtime = verify_runtime_log(log_path, exit_code)
    runtime.update(
        {
            "stage": "reload_home_audio",
            "changed_slots_read": sorted({item["slot"] for item in read_hits}),
            "reload_slot_mutations": "none",
            "nandsafe_tmp": "absent",
        }
    )
    return runtime


def self_test() -> dict[str, Any]:
    root = Path("build") / "qa_acceptance_selftest"
    if root.exists():
        shutil.rmtree(root)
    try:
        nand = root / "nand"
        nand.mkdir(parents=True)
        for index in range(4):
            (nand / f"sdat{index}.bin").write_bytes(bytes([index]) * 1600)
        baseline = inspect_nand(nand)
        baseline_path = root / "before.json"
        write_json(baseline_path, baseline)

        (nand / "sdat2.bin").write_bytes(b"A" * 1600)
        saved_path = root / "saved.json"
        saved_result = verify_save(baseline_path, nand, saved_path)
        if saved_result["changed_slots"] != ["sdat2.bin"]:
            fail("self-test did not identify changed slot")

        log_path = root / "reload.log"
        log_path.write_text(
            '[nand] accept ios_open fd=101 mode=1 path="...\\\\sdat2.bin"\n'
            '[nand] accept ios_read fd=101 length=1600 bytes=1600 path="...\\\\sdat2.bin"\n'
            "[audio-dbg] backend opened sdl=S16LE rate=32000Hz channels=2\n"
            "[audio-dbg] 1s: aiBlocks=+333 axFrames=+333 aiCallbacks=+333 nonSilentBlocks=+2 "
            "sdlBlocks=+333 sdlNonSilentBlocks=+2 sdlPeak=12345 backendOpens=1 "
            "voices=1 (started 1 stopped 0) peak=12345 rate=32000Hz dma=1 cb=1\n"
            "[os] OSReturnToMenu: guest requested exit, shutting down cleanly\n",
            encoding="utf-8",
        )
        verify_reload(baseline_path, saved_path, nand, log_path, 0)

        (nand / "sdat0.bin.nandsafe.tmp").write_bytes(b"partial")
        try:
            inspect_nand(nand)
        except ValueError:
            pass
        else:
            fail("self-test accepted an uncommitted NAND shadow")

        silent_log = root / "silent.log"
        silent_log.write_text(
            "[audio-dbg] backend opened sdl=S16LE rate=32000Hz channels=2\n"
            "[audio-dbg] 1s: sdlBlocks=+333 sdlNonSilentBlocks=+0 sdlPeak=0\n"
            "[os] OSReturnToMenu: guest requested exit, shutting down cleanly\n",
            encoding="utf-8",
        )
        try:
            verify_runtime_log(silent_log, 0)
        except ValueError:
            pass
        else:
            fail("self-test accepted silent SDL PCM")
        return {"status": "PASS", "checks": 6, "fixture": str(root)}
    finally:
        if root.exists():
            shutil.rmtree(root)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    snapshot = sub.add_parser("snapshot")
    snapshot.add_argument("--nand-dir", type=Path, required=True)
    snapshot.add_argument("--output", type=Path, required=True)
    snapshot.add_argument("--expected-size", type=int, default=1600)

    save = sub.add_parser("verify-save")
    save.add_argument("--before", type=Path, required=True)
    save.add_argument("--nand-dir", type=Path, required=True)
    save.add_argument("--output", type=Path)

    reload_ = sub.add_parser("verify-reload")
    reload_.add_argument("--before", type=Path, required=True)
    reload_.add_argument("--saved", type=Path, required=True)
    reload_.add_argument("--nand-dir", type=Path, required=True)
    reload_.add_argument("--log", type=Path, required=True)
    reload_.add_argument("--exit-code", type=int, required=True)

    runtime = sub.add_parser("verify-runtime")
    runtime.add_argument("--log", type=Path, required=True)
    runtime.add_argument("--exit-code", type=int, required=True)

    sub.add_parser("self-test")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        if args.command == "snapshot":
            write_json(args.output, inspect_nand(args.nand_dir, args.expected_size))
            result = {"status": "PASS", "manifest": str(args.output)}
        elif args.command == "verify-save":
            result = verify_save(args.before, args.nand_dir, args.output)
        elif args.command == "verify-reload":
            result = verify_reload(
                args.before, args.saved, args.nand_dir, args.log, args.exit_code
            )
        elif args.command == "verify-runtime":
            result = verify_runtime_log(args.log, args.exit_code)
        else:
            result = self_test()
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(json.dumps({"status": "FAIL", "error": str(error)}, indent=2), file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
