"""Exercise the actual packaging script using isolated official/Homebrew layouts."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

script = Path(__file__).resolve().parents[1] / "scripts/build_steam_service.sh"
mock_dotnet = '''#!/usr/bin/env python3
import json, os, pathlib, sys
args = sys.argv[1:]
if args == ["--list-runtimes"]:
    for version in ["10.0.9", "10.0.12", "9.0.99"]:
        print("Microsoft.NETCore.App " + version + " [" + os.environ["FIXTURE_SHARED"] + "]")
else:
    assert args[0] == "publish" and "" not in args, args
    pathlib.Path(os.environ["FIXTURE_ARGS"]).write_text(json.dumps(args))
    output = pathlib.Path(args[args.index("-o") + 1])
    output.mkdir(parents=True, exist_ok=True)
    (output / "MirageSteamService.dll").write_text("fixture")
'''

with tempfile.TemporaryDirectory(prefix="mirage-packaging-") as directory:
    base = Path(directory)
    root = base / "source"
    licenses = root / "SteamService/Licenses"
    licenses.mkdir(parents=True)
    for name in ["LGPL-2.1.txt", "SteamKit2-NOTICE.txt", "DepotDownloader-NOTICE.txt"]:
        (licenses / name).write_text(name)
    tools = base / "bin"
    tools.mkdir()
    (tools / "dotnet").write_text(mock_dotnet)
    (tools / "dotnet").chmod(0o755)
    (tools / "file").write_text('#!/bin/sh\nprintf "%s: Mach-O 64-bit executable arm64\\n" "$1"\n')
    (tools / "file").chmod(0o755)
    env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"])
    for layout in ["official", "homebrew"]:
        runtime = base / ("official runtime" if layout == "official" else "brew formula/libexec")
        shared = runtime / "shared/Microsoft.NETCore.App"
        for version in ["10.0.9", "10.0.12"]:
            (shared / version).mkdir(parents=True)
            (shared / version / "coreclr.dylib").write_text(version)
            (runtime / "host/fxr" / version).mkdir(parents=True)
        (runtime / "dotnet").write_text(mock_dotnet)
        (runtime / "dotnet").chmod(0o755)
        documentation = runtime if layout == "official" else runtime.parent / "share/doc/dotnet"
        documentation.mkdir(parents=True, exist_ok=True)
        for name in ["LICENSE.txt", "ThirdPartyNotices.txt"]:
            (documentation / name).write_text(layout + name)
        calls = base / "publish-args.json"
        env.update(FIXTURE_SHARED=str(shared), FIXTURE_ARGS=str(calls))
        for ci in ["false", "true"]:
            app = base / (layout + " " + ci + ".app")
            env["CI"] = ci
            subprocess.run(["/bin/bash", str(script), str(app), str(root), "arm64"],
                           env=env, check=True, timeout=20)
            args = json.loads(calls.read_text())
            assert ("-p:RestoreLockedMode=true" in args) == (ci == "true")
            bundled = app / "Contents/Resources/SteamService"
            assert (bundled / "arm64/runtime/shared/Microsoft.NETCore.App/10.0.12/coreclr.dylib").exists()
            assert (bundled / "Licenses/dotnet-LICENSE.txt").read_text() == layout + "LICENSE.txt"
            assert (bundled / "Licenses/dotnet-ThirdPartyNotices.txt").read_text() == layout + "ThirdPartyNotices.txt"
            print("PASS:", layout, "CI=" + ci, "paths with spaces, version selection, licenses and Bash empty arrays")
        (documentation / "LICENSE.txt").unlink()
        app = base / (layout + " false.app")
        marker = app / "Contents/Resources/SteamService/keep.txt"
        marker.write_text("keep")
        result = subprocess.run(["/bin/bash", str(script), str(app), str(root), "arm64"],
                                env=env, text=True, capture_output=True, timeout=20)
        assert result.returncode != 0 and "license is unavailable" in result.stderr
        assert marker.read_text() == "keep"
        print("PASS:", layout, "missing-license failure preserves previous bundle")
