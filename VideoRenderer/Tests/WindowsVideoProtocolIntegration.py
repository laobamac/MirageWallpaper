import json
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import time


def run(binary, ffmpeg):
    with tempfile.TemporaryDirectory(prefix="mirage-video-中文-") as directory:
        root = pathlib.Path(directory)
        video = root / "video.mp4"
        subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
                        "-f", "lavfi", "-i", "testsrc2=size=480x120:rate=30",
                        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
                        "-t", "2", "-c:v", "libx264", "-pix_fmt", "yuv420p",
                        "-c:a", "aac", str(video)], check=True, timeout=20)
        (root / "project.json").write_text(json.dumps({"type": "video", "file": "video.mp4"}), encoding="utf-8")
        for memory in (False, True):
            events = queue.Queue()
            stderr = []
            args = [binary, str(root), "--windowed", "--control-stdin", "--fps", "30", "--run-seconds", "15"]
            if memory:
                args.append("--load-from-memory")
            process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, encoding="utf-8")
            def read_stdout():
                for line in process.stdout:
                    try:
                        events.put(json.loads(line))
                    except json.JSONDecodeError:
                        pass
                events.put({"event": "process-exit"})
            def read_stderr():
                stderr.extend(process.stderr)
            stdout_thread = threading.Thread(target=read_stdout, daemon=True)
            stderr_thread = threading.Thread(target=read_stderr, daemon=True)
            stdout_thread.start()
            stderr_thread.start()
            def send(cmd, **values):
                process.stdin.write(json.dumps({"cmd": cmd, **values}) + "\n")
                process.stdin.flush()
            def wait(name, token=None, timeout=7):
                deadline = time.monotonic() + timeout
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise AssertionError(f"missing {name}; stderr={''.join(stderr)}")
                    event = events.get(timeout=remaining)
                    if event.get("event") == "process-exit":
                        raise AssertionError(f"renderer exited before {name}; stderr={''.join(stderr)}")
                    if event.get("event") == name and (token is None or event.get("token") == token):
                        return event
            def snapshot(name):
                path = root / f"{name}-{memory}.png"
                send("snapshot", path=str(path), token=name)
                assert wait("snapshot-done", name)["ok"], "snapshot command failed"
                assert path.stat().st_size > 100, "empty GPU snapshot"
                return subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-i", str(path),
                                       "-vf", "scale=32:32", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                                      check=True, stdout=subprocess.PIPE, timeout=10).stdout
            try:
                wait("first-frame-presented")
                wait("video-did-end")
                send("capabilities")
                capability = wait("renderer-capabilities")
                assert capability["gpu"] and capability["audio"] and "snapshot" in capability["commands"]
                send("not-a-video-command")
                assert wait("unsupported-command")["supported"] is False
                send("pause")
                send("volume", value=0.4)
                send("muted", value=True)
                send("position", x=0.0, y=0.5)
                time.sleep(0.25)
                left = snapshot("left")
                assert max(left) > 40, "presented framebuffer is black"
                send("position", x=1.0, y=0.5)
                time.sleep(0.25)
                right = snapshot("right")
                assert left != right, "crop anchor did not change GPU frame"
                send("fillmode", value="contain")
                time.sleep(0.25)
                contain = snapshot("contain")
                assert contain != right, "fillmode did not change GPU frame"
                send("speed", value=1.5)
                send("seek", value=0.5)
                send("muted", value=False)
                send("power", state="throttle")
                send("power", state="pause")
                send("resume")
                send("quit")
                process.wait(timeout=5)
                stdout_thread.join(timeout=2)
                stderr_thread.join(timeout=2)
                assert process.returncode == 0, ''.join(stderr)
                assert not any("control:" in line for line in stderr), ''.join(stderr)
                assert any("AO: [wasapi]" in line for line in stderr), "WASAPI audio output was not initialized"
                renderer_lines = [line.lower() for line in stderr if "OpenGL renderer:" in line]
                assert renderer_lines and not any("gdi generic" in line or "llvmpipe" in line for line in renderer_lines), "no native GPU OpenGL renderer"
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
        # 初始速度必须来自命令行：崩溃恢复或重新应用时不能退回 1.0。
        def playback_seconds(speed):
            started = None
            events = queue.Queue()
            args = [binary, str(root), "--windowed", "--control-stdin", "--fps", "30",
                    "--run-seconds", "20", "--speed", repr(speed)]
            process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, encoding="utf-8")
            def pump():
                for line in process.stdout:
                    try:
                        events.put(json.loads(line))
                    except json.JSONDecodeError:
                        pass
                events.put({"event": "process-exit"})
            pump_thread = threading.Thread(target=pump, daemon=True)
            pump_thread.start()
            try:
                deadline = time.monotonic() + 25
                while True:
                    event = events.get(timeout=max(0.1, deadline - time.monotonic()))
                    name = event.get("event")
                    if name == "process-exit":
                        raise AssertionError("renderer exited before finishing playback")
                    if name == "first-frame-presented" and started is None:
                        started = time.monotonic()
                    elif name == "video-did-end" and started is not None:
                        return time.monotonic() - started
            finally:
                if process.poll() is None:
                    try:
                        process.stdin.write(json.dumps({"cmd": "quit"}) + "\n")
                        process.stdin.flush()
                    except (BrokenPipeError, OSError, ValueError):
                        pass
                    process.wait(timeout=5)
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
                pump_thread.join(timeout=2)

        slow = playback_seconds(0.25)
        fast = playback_seconds(4.0)
        assert fast * 4.0 < slow, f"initial speed ignored: 0.25x={slow:.2f}s 4x={fast:.2f}s"
        rejected = subprocess.run([binary, str(video), "--windowed", "--control-stdin", "--speed", "0"],
                                  input=b"", stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        assert rejected.returncode == 1, "non-positive initial speed must be rejected"
        # 保持父进程写管道开启且无输入，验证定时退出会取消 stdin，而非卡在 ReadFile。
        process = subprocess.Popen([binary, str(video), "--windowed", "--control-stdin", "--run-seconds", "1"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            process.wait(timeout=5)
            assert process.returncode == 0, process.stderr.read().decode(errors="replace")
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
        # EOF 与命令退出必须同样结束 GUI 事件循环。
        result = subprocess.run([binary, str(video), "--windowed", "--control-stdin"],
                                input=b"", stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        assert result.returncode == 0, result.stderr.decode(errors="replace")


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
