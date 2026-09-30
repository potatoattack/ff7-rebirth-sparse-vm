#!/usr/bin/env python3
"""Capture a warmed, 40-second Rebirth comparison without a logging hotkey."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
import time

import mango_config
from plot import metrics, read_frames

STATE = Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local/state"))) / "ff7-sparse-vm-benchmarks"
MARKER = "FF7_SPARSE_VM_BENCHMARK"
ACTIVATION = "radv: sparse_vm v1 enabled for this device VM"
PACKAGES = ["linux-cachyos-sparse-vm", "vulkan-radeon-sparse-vm", "mangohud"]
ENV_KEYS = ["PROTON_ENABLE_WAYLAND", "WINE_GRAPHICS_DRIVER", "DXVK_HDR",
            "RADV_EXPERIMENTAL", "RADV_DEBUG", "AMD_VULKAN_ICD", "VKD3D_CONFIG",
            "PROTON_FSR4_RDNA3_UPGRADE", "PROTON_XESS_UPGRADE", "WINE_UPSCALER_REPLACE"]


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def package_versions():
    return subprocess.check_output(["pacman", "-Q", *PACKAGES], text=True,
                                   stderr=subprocess.STDOUT, timeout=10).strip()


def launch(mode, label, command):
    if command[:1] == ["--"]:
        command = command[1:]
    if not command or not re.fullmatch(r"[a-zA-Z0-9_-]{1,40}", label):
        raise RuntimeError("Use launch off|on LABEL -- %command%; LABEL uses letters, digits, - or _.")
    env = dict(os.environ)
    legacy = [key for key in env if key.startswith(("RADV_FF7_", "FF7_E"))]
    if legacy or env.get("VKD3D_QUEUE_PROFILE") or env.get("VK_INSTANCE_LAYERS"):
        raise RuntimeError("Benchmark needs a direct packaged-driver launch without legacy experiment or profiling layers.")
    if env.get("VK_DRIVER_FILES") or env.get("VK_ICD_FILENAMES"):
        raise RuntimeError("Benchmark the installed driver; remove a private ICD override for this launch.")
    kernel = os.uname().release
    if not kernel.endswith("-cachyos-sparse-vm"):
        raise RuntimeError("Boot the repository's packaged kernel first; current kernel is " + kernel)
    versions = package_versions()
    tokens = [token for token in env.get("RADV_EXPERIMENTAL", "").split(",") if token and token != "sparse_vm"]
    if mode == "on":
        tokens.append("sparse_vm")
    if tokens:
        env["RADV_EXPERIMENTAL"] = ",".join(tokens)
    else:
        env.pop("RADV_EXPERIMENTAL", None)
    directory = Path(tempfile.mkdtemp(prefix=label + "-", dir=STATE))
    if any(char in str(directory) for char in ",\n"):
        raise RuntimeError("MangoHud output path must not contain commas or newlines.")
    config = mango_config.create(directory)
    env.update({MARKER: str(directory), "MANGOHUD": "1", "MANGOHUD_CONFIGFILE": str(config),
                "MANGOHUD_CONFIG": "read_cfg,fps,frametime,frame_timing,gpu_stats,"
                "log_interval=0,log_duration=40,permit_upload=0,output_folder=" + str(directory)})
    meta = {"mode": mode, "label": label, "kernel": kernel,
            "packages": versions, "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
            "launch_monotonic_ns": time.monotonic_ns(),
            "installed_driver_sha256": digest(Path("/usr/lib/libvulkan_radeon.so")),
            "collector_sha256": digest(Path(__file__)), "complete": False}
    write_json(directory / "launch.json", meta)
    temporary = STATE / "latest.tmp"
    temporary.write_text(str(directory))
    temporary.replace(STATE / "latest")
    print("Benchmark output: " + str(directory), flush=True)
    with (directory / "launch-output.log").open("w") as log:
        os.dup2(log.fileno(), 1)
        os.dup2(log.fileno(), 2)
    os.execvpe(command[0], command, env)


def environment(proc):
    return dict(item.split("=", 1) for item in (proc / "environ").read_bytes().decode(errors="replace").split("\0") if "=" in item)


def game(directory):
    found = []
    for proc in Path("/proc").glob("[0-9]*"):
        try:
            if proc.stat().st_uid != os.getuid():
                continue
            env = environment(proc)
            if env.get(MARKER) != str(directory):
                continue
            maps = (proc / "maps").read_text()
            if re.search(r"/End/Binaries/Win64/ff7rebirth_\.exe(?:\s|$)", maps, re.I):
                found.append((proc, env, maps))
        except (OSError, ValueError):
            continue
    if len(found) != 1:
        raise RuntimeError(f"Load the save first; found {len(found)} mapped Rebirth processes for this launch.")
    return found[0]


def lifetime(proc):
    fields = (proc / "stat").read_text().rsplit(")", 1)[1].split()
    if fields[0] in ("Z", "X"):
        raise RuntimeError("Game process exited")
    return fields[19]


def snapshot(proc, env, maps):
    binaries = {}
    for line in maps.splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) != 6 or not fields[5].startswith("/"):
            continue
        name = Path(fields[5]).name
        if name not in {"libvulkan_radeon.so", "libMangoHud.so", "winewayland.so",
                        "winex11.so", "d3d12core.dll", "ff7rebirth_.exe", "ntdll.so"}:
            continue
        if name not in binaries:
            binaries[name] = digest(proc / "root" / fields[5].lstrip("/"))
    prefix = Path(env["WINEPREFIX"])
    config_root = proc / "root" / str(prefix).lstrip("/") / "drive_c/users/steamuser/Documents/My Games/FINAL FANTASY VII REBIRTH/Saved/Config"
    settings = {str(path.relative_to(config_root)): digest(path) for path in sorted(config_root.rglob("*.ini"))}
    if not settings:
        raise RuntimeError("No game settings found in the running prefix")
    text = {str(path.relative_to(config_root)): path.read_text(errors="replace")
            for path in sorted(config_root.rglob("*.ini")) if path.name in ("Engine.ini", "GameUserSettings.ini")}
    return {"binaries": binaries, "config_sha256": settings, "graphics_config": text,
            "environment": {key: env.get(key) for key in ENV_KEYS}}


def capture(note):
    directory = Path((STATE / "latest").read_text())
    meta = json.loads((directory / "launch.json").read_text())
    if (directory / "capture.json").exists():
        raise RuntimeError("This launch already has a capture attempt. Close and relaunch for another run.")
    if list(directory.glob("*.csv")):
        raise RuntimeError("Logging already started in this launch. Close and relaunch without using a logging hotkey.")
    proc, env, maps = game(directory)
    identity = lifetime(proc)
    config, watch = mango_config.verify(proc, env, directory)
    if not watch["matches"]:
        raise RuntimeError("MangoHud has no inotify watch on its private config; see launch-output.log.")
    before = snapshot(proc, env, maps)
    if before["binaries"].get("libvulkan_radeon.so") != meta["installed_driver_sha256"]:
        raise RuntimeError("The loaded driver differs from the installed packaged driver")
    if "libMangoHud.so" not in before["binaries"]:
        raise RuntimeError("MangoHud is not loaded in the game")
    activation = ACTIVATION in (directory / "launch-output.log").read_text(errors="replace")
    enabled = "sparse_vm" in env.get("RADV_EXPERIMENTAL", "").split(",")
    if enabled != (meta["mode"] == "on") or activation != enabled:
        raise RuntimeError("Flag and driver activation message do not match the requested mode")
    plan = mango_config.schedule(meta["launch_monotonic_ns"])
    result = {**meta, "note": note, "before": before, "activation_seen": activation,
              "plan": plan, "complete": False}
    write_json(directory / "capture.json", result)
    try:
        print("FF7_READY: focus Rebirth and rotate through the same view now.", flush=True)
        print("At least 30 seconds of warm-up, then 40 seconds of automatic logging. No hotkey.", flush=True)
        mango_config.write_trigger(config, 0, plan["autostart_log"])
        start = time.monotonic()
        deadline = start + plan["start_timeout_seconds"] + 55
        stable = None
        reported = -1
        while time.monotonic() < deadline:
            if lifetime(proc) != identity:
                raise RuntimeError("Game process changed during capture")
            csvs = [p for p in directory.glob("*.csv")
                    if "ff7rebirth" in p.name.lower() and not p.name.endswith("_summary.csv")]
            summaries = [p for p in directory.glob("*_summary.csv") if "ff7rebirth" in p.name.lower()]
            if len(csvs) > 1 or len(summaries) > 1:
                raise RuntimeError("Multiple game logs in a single capture")
            if len(csvs) == len(summaries) == 1:
                stamp = [(p.stat().st_size, p.stat().st_mtime_ns) for p in csvs + summaries]
                if stamp == stable:
                    frames, _ = read_frames(csvs[0])
                    measured = metrics(frames)
                    if not 39 <= measured["duration_s"] <= 42:
                        raise RuntimeError("MangoHud stopped before a full 40-second capture")
                    result.update(csv=csvs[0].name, csv_sha256=digest(csvs[0]), metrics=measured)
                    break
                stable = stamp
            elapsed = int((time.monotonic() - start) // 5) * 5
            if elapsed != reported:
                print(f"{elapsed}s since arming; keep rotating until capture completes.", flush=True)
                reported = elapsed
            time.sleep(1)
        else:
            raise RuntimeError("Timed out waiting for a complete MangoHud log")
        result["after"] = snapshot(proc, environment(proc), (proc / "maps").read_text())
        if result["after"] != before:
            raise RuntimeError("Settings, selected environment or loaded binaries changed during capture")
        result["complete"] = True
        print("Capture complete. You can close the game now.", flush=True)
    except (OSError, ValueError, KeyError, RuntimeError, KeyboardInterrupt) as error:
        result["error"] = str(error) or "Interrupted"
        raise
    finally:
        write_json(directory / "capture.json", result)
        if not result["complete"] and config.read_bytes() == mango_config.contents(plan["autostart_log"]):
            mango_config.write_trigger(config, plan["autostart_log"], 0)
        archive = directory.with_suffix(".tar.gz")
        with tarfile.open(archive, "w:gz") as output:
            for path in sorted(directory.iterdir()):
                if path.is_file() and not path.is_symlink() and path.name != "launch-output.log":
                    output.add(path, arcname=directory.name + "/" + path.name)
        print("Upload: " + str(archive), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    launcher = sub.add_parser("launch")
    launcher.add_argument("mode", choices=["off", "on"])
    launcher.add_argument("label")
    launcher.add_argument("command", nargs=argparse.REMAINDER)
    recorder = sub.add_parser("capture")
    recorder.add_argument("--note", required=True, help="Scene, resolution/scaling, texture quality, cap, controller motion")
    args = parser.parse_args()
    STATE.mkdir(parents=True, exist_ok=True)
    if args.action == "launch":
        launch(args.mode, args.label, args.command)
    else:
        capture(args.note)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        message = "FF7 benchmark stopped: " + str(error)
        print(message, file=sys.stderr)
        STATE.mkdir(parents=True, exist_ok=True)
        (STATE / "last-error.txt").write_text(message + "\n")
        sys.exit(1)
