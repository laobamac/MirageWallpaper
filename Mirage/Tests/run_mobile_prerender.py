#!/usr/bin/env python3
"""Compile the production options and device model in a standalone regression runner."""
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="mirage-mobile-prerender-") as directory:
    directory = Path(directory)
    model = (root / "Mirage/Mirage Wallpaper/ContentView/Components/MobileDevicesView.swift").read_text()
    model = "import Foundation\n" + model[model.index("struct MobileDevice:"):model.index("enum MobileDevicesScreen:")]
    (directory / "MobileDevice.swift").write_text(model)
    (directory / "Support.swift").write_text('''import Foundation
func L(_ value: String) -> String { value }
enum SceneMobileExportError: Error { case invalidProject }
enum WallpaperBakeError: Error { case code(String) }
''')
    binary = directory / "regression"
    subprocess.run(["xcrun", "swiftc", "-o", str(binary),
        str(root / "Mirage/Mirage Wallpaper/Services/SceneMobileExportOptions.swift"),
        str(root / "Mirage/Mirage Wallpaper/Services/SceneMobileTiming.swift"),
        str(directory / "MobileDevice.swift"), str(directory / "Support.swift"),
        str(root / "Mirage/Tests/MobilePreRenderRegression.swift")], check=True)
    subprocess.run([str(binary)], check=True)
