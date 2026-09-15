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
    parser.add_argument("--playback-policy", action="store_true")
    parser.add_argument("--stopped-displays", action="store_true")
    parser.add_argument("--configuration", choices=["Debug", "Release"], default="Debug")
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--baseline-api", action="store_true")
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
                "-scheme", "Mirage Wallpaper", "-configuration", args.configuration,
                "-derivedDataPath", str(args.derived_data),
                "-destination", f"platform=macOS,arch={platform.machine()}",
                "-onlyUsePackageVersionsFromResolvedFile", "CODE_SIGNING_ALLOWED=NO",
                "ONLY_ACTIVE_ARCH=YES", "ENABLE_TESTABILITY=YES", "ENABLE_DEBUG_DYLIB=YES", "build"
            ], stdout=log, stderr=subprocess.STDOUT, env=env)
        if result.returncode:
            raise SystemExit(f"Build failed; see {artifacts / 'build.log'}")
    products = args.derived_data / "Build/Products" / args.configuration
    app = products / "Mirage Wallpaper.app/Contents"
    library = app / "MacOS/Mirage Wallpaper.debug.dylib"
    if not library.exists():
        library = artifacts / "libMirageUITesting.dylib"
        objects = (args.derived_data / "Build/Intermediates.noindex/Mirage Wallpaper.build" /
                   args.configuration / "Mirage Wallpaper.build/Objects-normal" /
                   platform.machine() / "Mirage Wallpaper.LinkFileList")
        subprocess.run([
            "xcrun", "swiftc", "-emit-library", "-profile-generate", "-target",
            f"{platform.machine()}-apple-macos14.2", "-F", str(products),
            "-F", str(products / "PackageFrameworks"),
            *objects.read_text().splitlines(),
            "-o", str(library)
        ], check=True, env=env)
    name = "UIInteractionBenchmark" if args.benchmark else "UIResponsivenessRegression"
    executable = artifacts / name
    subprocess.run([
        "xcrun", "swiftc", "-parse-as-library", "-target",
        f"{platform.machine()}-apple-macos14.2", "-I", str(products),
        "-F", str(products), "-F", str(products / "PackageFrameworks"),
        *( ["-O"] if args.configuration == "Release" else [] ),
        *( ["-D", "MIRAGE_UI_BASELINE"] if args.baseline_api else [] ),
        str(project / "Tests" / (name + ".swift")),
        str(library),
        "-Xlinker", "-rpath", "-Xlinker", str(app / "MacOS"),
        "-Xlinker", "-rpath", "-Xlinker", str(app / "Frameworks"),
        "-o", str(executable)
    ], check=True, env=env)
    with (artifacts / "renderer.log").open("w") as log:
        command = [str(executable)]
        if args.startup_playlist:
            command.append("--startup-playlist")
        if args.playback_policy:
            command.append("--playback-policy")
        if args.stopped_displays:
            command.append("--stopped-displays")
        result = subprocess.run(command, cwd=artifacts, env=env,
                                stderr=log, timeout=90)
    if result.returncode:
        print((artifacts / "renderer.log").read_text(), flush=True)
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
