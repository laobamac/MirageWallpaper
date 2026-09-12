#
#  Mirage Wallpaper
#
#  Copyright © 2026 王孝慈. All rights reserved.
#

import argparse
import os
from pathlib import Path
import platform
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--derived-data", type=Path,
                        default=Path(tempfile.gettempdir()) / "MirageUIRegressionBuild")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--startup-playlist", action="store_true")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    artifacts = Path(tempfile.mkdtemp(prefix="mirage-ui-regression-"))
    print(f"Artifacts: {artifacts}", flush=True)
    env = dict(os.environ)
    env["LLVM_PROFILE_FILE"] = str(artifacts / "coverage-%p.profraw")
    if not args.skip_build:
        with (artifacts / "build.log").open("w") as log:
            result = subprocess.run([
                "xcodebuild", "-project", str(project / "Mirage Wallpaper.xcodeproj"),
                "-scheme", "Mirage Wallpaper", "-configuration", "Debug",
                "-derivedDataPath", str(args.derived_data),
                "-destination", f"platform=macOS,arch={platform.machine()}",
                "-onlyUsePackageVersionsFromResolvedFile", "CODE_SIGNING_ALLOWED=NO",
                "ONLY_ACTIVE_ARCH=YES", "build"
            ], stdout=log, stderr=subprocess.STDOUT, env=env)
        if result.returncode:
            raise SystemExit(f"Build failed; see {artifacts / 'build.log'}")
    products = args.derived_data / "Build/Products/Debug"
    app = products / "Mirage Wallpaper.app/Contents"
    executable = artifacts / "UIResponsivenessRegression"
    subprocess.run([
        "xcrun", "swiftc", "-parse-as-library", "-target",
        f"{platform.machine()}-apple-macos14.2", "-I", str(products),
        "-F", str(products), "-F", str(products / "PackageFrameworks"),
        str(project / "Tests/UIResponsivenessRegression.swift"),
        str(app / "MacOS/Mirage Wallpaper.debug.dylib"),
        "-Xlinker", "-rpath", "-Xlinker", str(app / "MacOS"),
        "-Xlinker", "-rpath", "-Xlinker", str(app / "Frameworks"),
        "-o", str(executable)
    ], check=True, env=env)
    with (artifacts / "renderer.log").open("w") as log:
        command = [str(executable)]
        if args.startup_playlist:
            command.append("--startup-playlist")
        result = subprocess.run(command, cwd=artifacts, env=env,
                                stderr=log, timeout=90)
    if result.returncode:
        print((artifacts / "renderer.log").read_text(), flush=True)
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
