#!/usr/bin/env python3
"""Audit a staged ready-to-run package without launching it."""

import argparse
import hashlib
import json
import mmap
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def same_file(left, right):
    left, right = Path(left), Path(right)
    if not left.is_file() or not right.is_file():
        return False
    return left.stat().st_size == right.stat().st_size and sha256(left) == sha256(right)


def audit(source, package, modules, require_launcher_clean=False):
    source, package, modules = Path(source), Path(package), Path(modules)
    errors = []
    warnings = []
    checks = {}

    source_exe = source / "sinandpun.exe"
    package_exe = package / "sinandpun.exe"
    checks["exe_matches_build"] = same_file(source_exe, package_exe)
    if not checks["exe_matches_build"]:
        errors.append(f"Executable {package_exe.name} is missing, stale, or differs from the build source")

    for asset in ("dsp_coef.bin", "initial_pipeline_cache.db"):
        checks[asset] = same_file(source / asset, package / asset)
        if not checks[asset]:
            errors.append(f"{asset} is missing, stale, or differs from the build source")

    dll_names = sorted(path.name for path in source.glob("*.dll") if path.is_file())
    missing_dlls = [name for name in dll_names if not same_file(source / name, package / name)]
    checks["runtime_dlls"] = dll_names
    if missing_dlls:
        errors.append("stale or missing runtime DLLs: " + ", ".join(missing_dlls))
    optional_dlls = {
        "sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "sl.dlss_g.dll",
        "amd_fidelityfx_upscaler_dx12.dll", "amd_fidelityfx_loader_dx12.dll",
        "sl.pcl.dll", "sl.reflex.dll", "nvngx_dlss.dll", "nvngx_dlssg.dll",
    }
    extra_dlls = sorted(
        path.name for path in package.glob("*.dll")
        if path.name not in dll_names and path.name not in optional_dlls
    )
    if extra_dlls:
        errors.append("unexpected DLLs in package: " + ", ".join(extra_dlls))

    expected_modules = [f"stg0{index}.bin" for index in range(8)]
    bad_modules = [name for name in expected_modules if not same_file(modules / name, package / "modules" / name)]
    checks["stage_modules"] = len(expected_modules) - len(bad_modules)
    if require_launcher_clean and (package / "modules").exists():
        errors.append("launcher release contains disc-derived stage modules; generate them during setup")
    if bad_modules and not require_launcher_clean:
        errors.append("stale or missing stage modules: " + ", ".join(bad_modules))
    extra_modules = sorted(
        path.name for path in (package / "modules").glob("stg0?.bin")
        if path.name not in expected_modules
    )
    if extra_modules:
        errors.append("unexpected stage modules: " + ", ".join(extra_modules))

    if source.joinpath("wii_bootstrap").exists():
        source_bootstrap = sorted(
            path.relative_to(source / "wii_bootstrap").as_posix()
            for path in (source / "wii_bootstrap").rglob("*") if path.is_file()
            and not (require_launcher_clean and path == source / "wii_bootstrap/README.md")
        )
        package_bootstrap = package / "wii_bootstrap"
        missing_bootstrap = [
            name for name in source_bootstrap
            if not same_file(source / "wii_bootstrap" / name, package_bootstrap / name)
        ]
        checks["bootstrap_files"] = len(source_bootstrap) - len(missing_bootstrap)
        if missing_bootstrap:
            errors.append("stale or missing bootstrap files: " + ", ".join(missing_bootstrap))
    else:
        warnings.append("source build has no wii_bootstrap directory")

    if not same_file(ROOT / "LICENSE", package / "LICENSE"):
        errors.append("LICENSE is missing or differs from the source tree")

    manifest = package / "recomplauncher.json"
    if not same_file(ROOT / "recomplauncher.json", manifest):
        errors.append("recomplauncher.json is missing, stale, or differs from the source tree")
    elif manifest.is_file():
        try:
            profile = json.loads(manifest.read_text(encoding="utf-8"))
            checks["launcher_format"] = profile.get("format")
            if require_launcher_clean and any(profile.get(mode, {}).get("program") != ["sinandpun.exe"]
                                              for mode in ("start", "prepare")):
                errors.append("RecompLauncher start and prepare must use sinandpun.exe")
            if profile.get("format") != 1:
                errors.append("recomplauncher.json has an unsupported format")
            if profile.get("repo") != "meathax/sinandpun":
                errors.append("recomplauncher.json names the wrong repository")
        except json.JSONDecodeError as error:
            errors.append(f"recomplauncher.json is invalid JSON: {error}")

    if require_launcher_clean:
        if sorted(p.relative_to(package).as_posix() for p in package.rglob("*.exe")) != ["sinandpun.exe"]:
            errors.append("release must contain exactly one executable at the root: sinandpun.exe")
        if package_exe.is_file():
            # These diagnostics are emitted only by the native SDK dispatch code.
            if package_exe.stat().st_size:
                with package_exe.open("rb") as exe, mmap.mmap(exe.fileno(), 0, access=mmap.ACCESS_READ) as image:
                    if image.find(b"AMD FSR provider: %s") < 0 or image.find(b"slEvaluateFeature") < 0:
                        errors.append("release executable lacks the native FSR or DLSS backend")
        graphics = json.loads((ROOT / "tools/graphics_runtime.json").read_text())
        for name, expected in graphics.items():
            path = package / name
            if not path.is_file() or sha256(path).lower() != expected:
                errors.append(f"missing or incorrect graphics runtime file: {name}")
        mutable = [name for name in ("UserData", "recompdata", "portable.txt")
                   if (package / name).exists()]
        if mutable:
            errors.append("launcher release contains mutable or standalone-only state: " +
                          ", ".join(mutable))

    if not require_launcher_clean:
        build_info = package / "BUILD-INFO.txt"
        if package_exe.is_file() and build_info.is_file():
            executable_hash = sha256(package_exe)
            checks["build_info_hash_matches"] = executable_hash in build_info.read_text(
                encoding="utf-8", errors="replace"
            )
            if not checks["build_info_hash_matches"]:
                errors.append("BUILD-INFO.txt does not contain the packaged executable SHA-256")
        else:
            errors.append("BUILD-INFO.txt is missing")

    leaked = []
    for directory in ("sys", "files", "orig"):
        if (package / directory).exists():
            leaked.append(directory)
    leaked.extend(
        str(path.relative_to(package))
        for path in package.rglob("*")
        if path.is_file() and path.suffix.lower() in {".iso", ".nkit", ".rvz", ".wbfs", ".wia", ".wad", ".gcz", ".chd", ".dol", ".rso", ".sel"}
    )
    leaked.extend(
        str(path.relative_to(package))
        for path in package.rglob("*")
        if path.is_file() and (
            path.suffix.lower() in {".key", ".keys", ".pem", ".7z", ".hex"} or path.name.lower() == "common-key"
            or "key" in path.stem.lower() and path.suffix.lower() in {".bin", ".dat", ".txt"}
            or path.name.lower() == "dolphintool.exe"
        )
    )
    if leaked:
        errors.append("possible game data in package: " + ", ".join(sorted(set(leaked))))

    test_binaries = sorted(path.name for path in package.glob("mkw_*_tests.exe"))
    if test_binaries:
        warnings.append("test binaries in package: " + ", ".join(test_binaries))

    return {"source": str(source), "package": str(package), "checks": checks,
            "warnings": warnings, "errors": errors}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", default="build/native")
    parser.add_argument("--package", default="ready")
    parser.add_argument("--modules", default="build/modules")
    parser.add_argument("--json-out")
    parser.add_argument("--require-launcher-clean", action="store_true")
    args = parser.parse_args()

    report = audit(ROOT / args.source, ROOT / args.package, ROOT / args.modules,
                   args.require_launcher_clean)
    if args.json_out:
        output = ROOT / args.json_out
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{report['package']}: {len(report['errors'])} errors, {len(report['warnings'])} warnings")
    for error in report["errors"]:
        print("ERROR: " + error)
    for warning in report["warnings"]:
        print("WARNING: " + warning)
    return 1 if report["errors"] else 0


if __name__ == "__main__":
    sys.exit(main())
