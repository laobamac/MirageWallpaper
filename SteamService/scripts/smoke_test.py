#!/usr/bin/env python3
"""Exercise the credential-free SteamService JSON-lines protocol."""

from __future__ import annotations

import json
import selectors
import subprocess
import sys
from pathlib import Path
from typing import Any


TIMEOUT_SECONDS = 10


def fail(message: str) -> None:
    raise AssertionError(message)


def read_message(process: subprocess.Popen[str]) -> dict[str, Any]:
    assert process.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    try:
        if not selector.select(TIMEOUT_SECONDS):
            fail(f"SteamService produced no response within {TIMEOUT_SECONDS} seconds")
        line = process.stdout.readline()
    finally:
        selector.close()

    if not line:
        fail(f"SteamService exited before responding (exit={process.poll()})")
    try:
        payload = json.loads(line)
    except json.JSONDecodeError as error:
        fail(f"SteamService produced invalid JSON: {error}: {line!r}")
    if not isinstance(payload, dict):
        fail(f"SteamService response is not an object: {payload!r}")
    return payload


def send(process: subprocess.Popen[str], payload: dict[str, Any] | str) -> dict[str, Any]:
    assert process.stdin is not None
    line = payload if isinstance(payload, str) else json.dumps(payload, separators=(",", ":"))
    process.stdin.write(line + "\n")
    process.stdin.flush()
    return read_message(process)


def expect_fields(payload: dict[str, Any], **expected: Any) -> None:
    for key, value in expected.items():
        if payload.get(key) != value:
            fail(f"expected {key}={value!r}, received {payload!r}")


def main() -> int:
    if len(sys.argv) != 3:
        print(
            f"usage: {Path(sys.argv[0]).name} <dotnet-host> <MirageSteamService.dll>",
            file=sys.stderr,
        )
        return 2

    dotnet_host = Path(sys.argv[1]).resolve()
    assembly = Path(sys.argv[2]).resolve()
    if not dotnet_host.is_file():
        print(f"bundled dotnet host not found: {dotnet_host}", file=sys.stderr)
        return 2
    if not assembly.is_file():
        print(f"SteamService assembly not found: {assembly}", file=sys.stderr)
        return 2

    process = subprocess.Popen(
        [str(dotnet_host), str(assembly)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )

    try:
        hello = send(process, {"command": "hello", "requestId": "hello-smoke"})
        expect_fields(
            hello,
            type="hello",
            requestId="hello-smoke",
            version="1.0.0",
            maxConcurrentDownloads=3,
        )

        pong = send(process, {"command": "ping", "requestId": "ping-smoke"})
        expect_fields(pong, type="pong", requestId="ping-smoke")

        malformed = send(process, "{")
        expect_fields(malformed, type="response", success=False)

        missing = send(process, {"requestId": "missing-command-smoke"})
        expect_fields(
            missing,
            type="response",
            requestId="missing-command-smoke",
            success=False,
            errorCode="INVALID_COMMAND",
        )

        unsupported = send(
            process,
            {"command": "notARealCommand", "requestId": "unsupported-smoke"},
        )
        expect_fields(
            unsupported,
            type="response",
            requestId="unsupported-smoke",
            success=False,
            errorCode="UNSUPPORTED_COMMAND",
        )

        shutdown = send(process, {"command": "shutdown", "requestId": "shutdown-smoke"})
        expect_fields(
            shutdown,
            type="response",
            requestId="shutdown-smoke",
            success=True,
        )

        process.wait(timeout=TIMEOUT_SECONDS)
        if process.returncode != 0:
            fail(f"SteamService exited with status {process.returncode}")
        print("SteamService IPC smoke test passed (6 commands).")
        return 0
    except Exception:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        assert process.stderr is not None
        stderr = process.stderr.read().strip()
        if stderr:
            print(f"SteamService stderr:\n{stderr}", file=sys.stderr)
        raise


if __name__ == "__main__":
    raise SystemExit(main())
