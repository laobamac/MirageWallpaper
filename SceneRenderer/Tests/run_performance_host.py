# Mirage Wallpaper
# Copyright © 2026 王孝慈. All rights reserved.

"""Exercise invisible desktop hosts, paused snapshots and clean shutdown."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time


class Usage(ctypes.Structure):
    # rusage_info_v0, declared by the macOS SDK in sys/resource.h.
    _fields_ = [("uuid", ctypes.c_uint8 * 16)] + [
        (name, ctypes.c_uint64) for name in
        ("user_time", "system_time", "package_wakeups", "interrupt_wakeups",
         "pageins", "wired_size", "resident_size", "physical_footprint",
         "start_time", "exit_time")]


def idle_sample(pid):
    if os.name == "nt":
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            return None
        def cpu_ticks():
            times = [wintypes.FILETIME() for _ in range(4)]
            if not kernel.GetProcessTimes(handle, *[ctypes.byref(value) for value in times]):
                return None
            return sum((value.dwHighDateTime << 32) | value.dwLowDateTime for value in times[2:])
        try:
            before = cpu_ticks()
            started = time.monotonic()
            time.sleep(2)
            after = cpu_ticks()
            if before is None or after is None:
                return None
            return {"seconds": round(time.monotonic() - started, 3),
                    "cpu_seconds": (after - before) / 10_000_000}
        finally:
            kernel.CloseHandle(handle)
    libproc = ctypes.CDLL("/usr/lib/libproc.dylib")
    libproc.proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
    before, after = Usage(), Usage()
    if libproc.proc_pid_rusage(pid, 0, ctypes.byref(before)):
        return None
    started = time.monotonic()
    time.sleep(2)
    if libproc.proc_pid_rusage(pid, 0, ctypes.byref(after)):
        return None
    return {"seconds": round(time.monotonic() - started, 3),
            "package_idle_wakeups": after.package_wakeups - before.package_wakeups,
            "interrupt_wakeups": after.interrupt_wakeups - before.interrupt_wakeups,
            "physical_footprint_bytes": after.physical_footprint}


def run(binary, assets, workspace, metalfx):
    mode = "metalfx" if metalfx else "surface"
    command = [str(binary), str(assets), str(workspace / "scene.json"),
               "--cache-path", str(workspace / "cache"), "--resolution", "512x288",
               "--fps", "30", "--muted", "--no-spectrum", "--deferred-show",
               "--control-stdin", "--run-seconds", "20"]
    if metalfx:
        command.append("--metalfx")
    env = os.environ.copy()
    env["SCENERENDERER_DIAGNOSTIC_CACHE"] = str(workspace / "pipeline-cache")
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", env=env)
    lines = []
    events = queue.Queue()

    def read():
        for line in process.stdout:
            lines.append(line)
            try:
                events.put(json.loads(line))
            except ValueError:
                pass

    reader = threading.Thread(target=read, daemon=True)
    reader.start()

    def wait_event(name, timeout=12, token=None):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                event = events.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if event.get("event") == name and (token is None or event.get("token") == token):
                return event
        raise RuntimeError(f"{mode}: missing {name}; exit={process.poll()}")

    def send(message):
        process.stdin.write(json.dumps(message) + "\n")
        process.stdin.flush()

    try:
        wait_event("first-frame-presented")
        time.sleep(0.2)
        usage = idle_sample(process.pid)
        send({"cmd": "pause"})
        time.sleep(0.2)
        snapshot = workspace / (f"{mode}.png" if os.name == "nt" else f"{mode}.heic")
        send({"cmd": "snapshot", "path": str(snapshot), "token": mode})
        event = wait_event("snapshot-done", 6)
        if not event.get("ok") or not snapshot.exists() or not snapshot.stat().st_size:
            raise RuntimeError(f"{mode}: paused snapshot failed: {event}")
        if os.name == "nt":
            send({"cmd": "unsupported-regression"})
            capability = wait_event("capability", 6)
            if capability.get("command") != "unsupported-regression" or capability.get("supported") is not False:
                raise RuntimeError(f"{mode}: unsupported command was not reported")
            if snapshot.read_bytes()[:8] != b"\x89PNG\r\n\x1a\n":
                raise RuntimeError(f"{mode}: snapshot is not PNG")
            send({"cmd": "exportScriptStorage", "token": "storage-regression"})
            storage = wait_event("script-storage", 6, "storage-regression")
            if not isinstance(storage.get("values"), dict):
                raise RuntimeError(f"{mode}: malformed script-storage reply")
            send({"cmd": "activate"})
            wait_event("activated", 6)
            send({"cmd": "deactivate"})
            wait_event("deactivated", 6)
        send({"cmd": "quit"})
        process.wait(timeout=5)
        if process.returncode:
            raise RuntimeError(f"{mode}: shutdown failed: {process.returncode}")
        reader.join(timeout=2)
        return {"mode": mode, "snapshot_bytes": snapshot.stat().st_size,
                "metalfx_active": any("MetalFX Spatial active" in line for line in lines),
                "idle_sample": usage,
                "exit_code": process.returncode}
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        reader.join(timeout=2)
        (workspace / f"{mode}.log").write_text("".join(lines))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("assets", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mirage-performance-host-") as temporary:
        workspace = Path(temporary)
        scene = {"camera": {}, "general": {"clearcolor": [0, 0, 0],
                 "orthogonalprojection": {"width": 512, "height": 288}},
                 "objects": [{"id": 1, "image": "models/util/solidlayer.json",
                              "origin": [256, 144, 0], "size": [512, 288],
                              "color": [0.2, 0.4, 0.6], "visible": True}]}
        (workspace / "scene.json").write_text(json.dumps(scene))
        try:
            results = [run(args.binary.resolve(), args.assets.resolve(), workspace, mode)
                       for mode in ((False,) if os.name == "nt" else (False, True))]
            print(json.dumps(results, indent=2))
        except Exception:
            for log in workspace.glob("*.log"):
                print(log.name, log.read_text())
            raise


if __name__ == "__main__":
    main()
