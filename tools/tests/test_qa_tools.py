import importlib.util
import shutil
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


qa_report = load_tool("qa_report")
module_coverage = load_tool("module_coverage")
release_audit = load_tool("release_audit")


class ModuleCoverageTests(unittest.TestCase):
    class Module:
        @staticmethod
        def in_text(address):
            return address >= 0x80000000

    def test_rejected_materialized_constant_is_suspect(self):
        self.assertFalse(module_coverage.is_actionable_gap(
            0x97E50000, self.Module(), {"lisaddi"}, "mid-function"))

    def test_translator_map_skip_is_actionable(self):
        self.assertTrue(module_coverage.is_actionable_gap(
            0x97E50000, self.Module(), {"map-skipped"}, "mid-function"))

    def test_missing_external_call_is_actionable(self):
        self.assertTrue(module_coverage.is_actionable_gap(
            0x70000000, self.Module(), {"bl-ext"}, "-"))


class QaReportTests(unittest.TestCase):
    def setUp(self):
        self.directory = ROOT / "build" / "tool-tests" / self.id().split(".")[-1]
        shutil.rmtree(self.directory, ignore_errors=True)
        self.directory.mkdir(parents=True)

    def tearDown(self):
        shutil.rmtree(self.directory, ignore_errors=True)

    def test_reports_perf_swap_and_log_growth(self):
        (self.directory / "err.log").write_text(
            "[runtime] [perf] fps=60.0 guest=60.0 present=120.0 ratio=2.00 "
            "speed=1.000 vi_time=1.000 mode=1 plan=120.0 slots=2 cpu=50% "
            "(of one core) ws=900MB\n"
            "[runtime] [perf-upscale] upscaler=fsr1 active=fsr1 factor=2.00 "
            "internal=1920x1080 output=3840x2160 scale=2.000\n"
            "[autostage] t=20.0 stage 1 load reached (F state 4, loadedId=1)\n"
            "[os] RSOUnLinkList: stg01 unlinked\n"
            "[autostage] t=30.0 stage 2 load reached (F state 4, loadedId=2)\n"
            "[autostage] t=40.0 stage 1 load reached (F state 4, loadedId=1)\n",
            encoding="utf-8",
        )
        (self.directory / "out.log").write_text("", encoding="utf-8")
        (self.directory / "summary.txt").write_text("run ended: timeout elapsed 50s\n", encoding="utf-16")
        (self.directory / "log_growth.csv").write_text(
            "elapsed_s,err_bytes,out_bytes,runtime_bytes\n"
            "0,100,0,0\n"
            "60,100,0,1048576\n",
            encoding="utf-8",
        )

        report = qa_report.analyze(self.directory)
        self.assertEqual(report["stages_reached"], [1, 2, 1])
        self.assertEqual(report["duration_s"], 50)
        self.assertEqual(report["guest_fps_avg"], 60.0)
        self.assertEqual(report["upscales"][-1]["output"], (3840, 2160))
        self.assertAlmostEqual(report["max_growth_mib_per_min"], 1.0)

    def test_fatal_markers_fail_validation(self):
        (self.directory / "err.log").write_text(
            "[runtime] not translated target 0xdeadbeef\n", encoding="utf-8")
        report = qa_report.analyze(self.directory)
        args = type("Args", (), {
            "sequence": None,
            "require_resolution": None,
            "min_perf_samples": 0,
            "min_guest_avg": 0.0,
            "max_growth_mib_per_min": 0.0,
            "no_errors": True,
        })()
        self.assertTrue(qa_report.validate(report, args))


class ReleaseAuditTests(unittest.TestCase):
    def setUp(self):
        self.base = ROOT / "build" / "tool-tests" / self.id().split(".")[-1]
        shutil.rmtree(self.base, ignore_errors=True)
        self.source = self.base / "source"
        self.package = self.base / "package"
        self.modules = self.base / "modules"
        (self.source / "wii_bootstrap").mkdir(parents=True)
        (self.package / "modules").mkdir(parents=True)
        self.modules.mkdir(parents=True)

        (self.source / "WiiCompiled.exe").write_bytes(b"executable")
        (self.package / "WiiCompiled.exe").write_bytes(b"executable")
        (self.source / "runtime.dll").write_bytes(b"dll")
        (self.package / "runtime.dll").write_bytes(b"dll")
        (self.source / "wii_bootstrap" / "boot.bin").write_bytes(b"boot")
        (self.package / "wii_bootstrap").mkdir()
        (self.package / "wii_bootstrap" / "boot.bin").write_bytes(b"boot")
        (self.package / "LICENSE").write_bytes((ROOT / "LICENSE").read_bytes())
        (self.package / "recomplauncher.json").write_bytes(
            (ROOT / "recomplauncher.json").read_bytes())
        for index in range(8):
            name = f"stg0{index}.bin"
            (self.modules / name).write_bytes(index.to_bytes(1, "little"))
            (self.package / "modules" / name).write_bytes(index.to_bytes(1, "little"))
        executable_hash = release_audit.sha256(self.package / "WiiCompiled.exe")
        (self.package / "BUILD-INFO.txt").write_text(
            f"SHA-256 : {executable_hash}\n", encoding="utf-8")

    def tearDown(self):
        shutil.rmtree(self.base, ignore_errors=True)

    def test_clean_package_passes(self):
        report = release_audit.audit(self.source, self.package, self.modules)
        self.assertEqual(report["errors"], [])

    def test_stale_executable_and_game_data_fail(self):
        (self.package / "WiiCompiled.exe").write_bytes(b"old")
        (self.package / "sys").mkdir()
        report = release_audit.audit(self.source, self.package, self.modules)
        self.assertTrue(any("executable" in error.lower() for error in report["errors"]))
        self.assertTrue(any("game data" in error.lower() for error in report["errors"]))

    def test_launcher_package_rejects_mutable_state(self):
        (self.package / "UserData").mkdir()
        report = release_audit.audit(
            self.source, self.package, self.modules, require_launcher_clean=True)
        self.assertTrue(any("mutable" in error.lower() for error in report["errors"]))


if __name__ == "__main__":
    unittest.main()
