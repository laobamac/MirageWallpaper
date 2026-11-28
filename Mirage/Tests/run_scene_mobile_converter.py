#!/usr/bin/env python3
"""Compile and run the standalone C++20 SceneMobileConverter regression suite."""
# Copyright © 2026 王孝慈. All rights reserved.

import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONVERTER_DIR = ROOT / "Mirage/Mirage Wallpaper/Services/SceneMobileConverter"
VENDOR_DIR = ROOT / "SceneRenderer/Vendor"
JSON_INCLUDE = VENDOR_DIR / "nlohmann_json/single_include"
TEST_SRC = ROOT / "Mirage/Tests/SceneMobileConverterRegression.cpp"


def main() -> None:
    sources = sorted(CONVERTER_DIR.glob("*.cpp"))
    assert sources, f"No C++ sources found in {CONVERTER_DIR}"
    with tempfile.TemporaryDirectory(prefix="mirage-scene-mobile-cpp-") as tmp:
        binary = Path(tmp) / "scene_mobile_regression"
        cmd = [
            "xcrun",
            "clang++",
            "-std=gnu++20",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Wshorten-64-to-32",
            "-Werror",
            f"-I{CONVERTER_DIR}",
            f"-isystem{VENDOR_DIR}",
            f"-isystem{JSON_INCLUDE}",
            *(str(s) for s in sources),
            str(TEST_SRC),
            "-lcompression",
            "-framework",
            "Accelerate",
            "-framework",
            "CoreFoundation",
            "-framework",
            "CoreGraphics",
            "-framework",
            "ImageIO",
            "-o",
            str(binary),
        ]
        subprocess.run(cmd, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
