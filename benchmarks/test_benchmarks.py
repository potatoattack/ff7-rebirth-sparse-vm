"""Host checks for the recorder handoff and public frame-data calculations."""

import ctypes
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import unittest
from contextlib import ExitStack, redirect_stdout
import io
import shutil
from unittest.mock import patch

import capture
import mango_config
import plot

ROOT = Path(__file__).resolve().parent


class FrameData(unittest.TestCase):
    def test_audited_e07_results(self):
        for name, count, fps, p99, long_frames in [
                ("narrow", 3976, 99.87511415774533, 26.14445, 49),
                ("cross", 4396, 110.26046300949858, 21.01091, 10)]:
            frames, elapsed = plot.read_frames(ROOT / "examples/e07" / (name + ".csv"))
            result = plot.metrics(frames)
            self.assertEqual(result["frames"], count)
            self.assertAlmostEqual(result["average_fps"], fps)
            self.assertAlmostEqual(result["p99_ms"], p99)
            self.assertEqual(result["over_25_ms"], long_frames)
            self.assertEqual(elapsed[0], 0)

    def test_reject_invalid_or_sampled_logs(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "data.csv"
            for rows in ["0,10\n0,10\n", "0,10\n10000000,nan\n",
                         "0,10\n100000000,10\n", "0,10\n10000000\n"]:
                path.write_text("elapsed_ns,frametime_ms\n" + rows)
                with self.assertRaises(ValueError):
                    plot.read_frames(path)


class LoggingTrigger(unittest.TestCase):
    def test_real_inotify_and_in_place_write(self):
        with tempfile.TemporaryDirectory() as temp:
            path = mango_config.create(Path(temp))
            original = path.stat()
            libc = ctypes.CDLL(None, use_errno=True)
            fd = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
            self.assertGreaterEqual(fd, 0)
            try:
                self.assertGreaterEqual(libc.inotify_add_watch(fd, os.fsencode(path), 2), 0)
                self.assertTrue(mango_config.watched_config(Path("/proc/self"), path)["matches"])
                mango_config.write_trigger(path, 0, 180)
                self.assertEqual(path.stat().st_ino, original.st_ino)
                self.assertEqual(path.stat().st_size, original.st_size)
                self.assertTrue(select.select([fd], [], [], 1)[0])
                self.assertEqual(path.read_bytes(), mango_config.contents(180))
                with self.assertRaises(RuntimeError):
                    mango_config.write_trigger(path, 0, 181)
            finally:
                os.close(fd)

    def test_conservative_warmup_and_symlink_refusal(self):
        plan = mango_config.schedule(20_000_000_000, 50_700_000_000)
        self.assertGreaterEqual(plan["earliest_start_monotonic_ns"] - 50_700_000_000, 32e9)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = mango_config.create(root)
            alias = root / "alias"
            alias.symlink_to(target)
            with self.assertRaises(OSError):
                mango_config.write_trigger(alias, 0, 1)
            self.assertEqual(target.read_bytes(), mango_config.contents())

    def test_environment_override_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            config = mango_config.create(directory)
            env = {"MANGOHUD_CONFIGFILE": str(config), "MANGOHUD_CONFIG": "read_cfg,autostart_log=1"}
            with self.assertRaises(RuntimeError):
                mango_config.verify(Path("/proc/self"), env, directory)


class LaunchHandoff(unittest.TestCase):
    def test_real_exec_preserves_arguments_and_selects_only_requested_mode(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = Path(temp) / "runner.py"
            runner.write_text("""import os,sys,types
from pathlib import Path
sys.path.insert(0,sys.argv[1])
import capture
capture.STATE=Path(sys.argv[2]);capture.STATE.mkdir(exist_ok=True)
capture.os.uname=lambda:types.SimpleNamespace(release='test-cachyos-sparse-vm')
capture.package_versions=lambda:'fixture packages'
capture.digest=lambda p:'fixture hash'
capture.launch(sys.argv[3],sys.argv[3],sys.argv[4:])
""")
            script = "import json,os,sys;print(json.dumps({'args':sys.argv[1:],'radv':os.getenv('RADV_EXPERIMENTAL'),'wayland':os.getenv('PROTON_ENABLE_WAYLAND'),'hud':os.getenv('MANGOHUD_CONFIG')}))"
            for mode in ("off", "on"):
                state = Path(temp) / mode
                env = {"PATH": os.environ["PATH"], "HOME": temp,
                       "RADV_EXPERIMENTAL": "other,sparse_vm", "PROTON_ENABLE_WAYLAND": "1"}
                result = subprocess.run([sys.executable, str(runner), str(ROOT), str(state), mode,
                                         "--", sys.executable, "-c", script, "path with spaces", "$literal"],
                                        env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                directory = Path((state / "latest").read_text())
                output = json.loads((directory / "launch-output.log").read_text())
                self.assertEqual(output["args"], ["path with spaces", "$literal"])
                self.assertEqual(output["radv"], "other,sparse_vm" if mode == "on" else "other")
                self.assertEqual(output["wayland"], "1")
                self.assertIn("log_interval=0", output["hud"])
                self.assertNotIn("fps_limit=", output["hud"])
                self.assertEqual((directory / "MangoHud.conf").read_bytes(), mango_config.contents())


class RecordingLifecycle(unittest.TestCase):
    def record(self, completed):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as patches:
            state = Path(temp)
            directory = state / "run"
            directory.mkdir()
            (state / "latest").write_text(str(directory))
            capture.write_json(directory / "launch.json", {
                "mode": "off", "installed_driver_sha256": "fixture-driver",
                "launch_monotonic_ns": 0,
            })
            (directory / "launch-output.log").write_text("")
            config = mango_config.create(directory)
            snapshot = {"binaries": {"libvulkan_radeon.so": "fixture-driver", "libMangoHud.so": "fixture-hud"}}
            clock = [0.0]
            def sleep(seconds):
                clock[0] += seconds
                if clock[0] == 1 and completed:
                    shutil.copy(ROOT / "examples/e07/cross.csv", directory / "ff7rebirth_test.csv")
                    (directory / "ff7rebirth_test_summary.csv").write_text("finished\n")
            patches.enter_context(patch.object(capture, "STATE", state))
            patches.enter_context(patch.object(capture, "game", return_value=(Path("/proc/self"), {}, "")))
            patches.enter_context(patch.object(capture, "lifetime", return_value="fixture-start"))
            patches.enter_context(patch.object(capture, "snapshot", return_value=snapshot))
            patches.enter_context(patch.object(mango_config, "verify", return_value=(config, {"matches": [True]})))
            patches.enter_context(patch.object(mango_config, "schedule", return_value={"autostart_log": 32, "start_timeout_seconds": 40}))
            patches.enter_context(patch.object(capture.time, "monotonic", side_effect=lambda: clock[0]))
            patches.enter_context(patch.object(capture.time, "sleep", side_effect=sleep))
            with redirect_stdout(io.StringIO()):
                if completed:
                    capture.capture("fixture recording; not game evidence")
                else:
                    with self.assertRaisesRegex(RuntimeError, "Timed out"):
                        capture.capture("fixture recording; not game evidence")
            result = json.loads((directory / "capture.json").read_text())
            self.assertEqual(result["complete"], completed)
            self.assertTrue(directory.with_suffix(".tar.gz").is_file())
            if not completed:
                self.assertIn("Timed out", result["error"])
                self.assertEqual(config.read_bytes(), mango_config.contents())
            return result

    def test_complete_log_is_hashed_and_archived(self):
        self.assertEqual(self.record(True)["metrics"]["frames"], 4396)

    def test_timeout_disarms_and_preserves_diagnostics(self):
        self.record(False)


if __name__ == "__main__":
    unittest.main()
