#
#  Mirage Wallpaper
#
#  Copyright © 2026 王孝慈. All rights reserved.
#

from pathlib import Path
import platform
import plistlib
import shutil
import subprocess
import tempfile
import uuid


def main():
    project = Path(__file__).resolve().parents[1]
    artifacts = Path(tempfile.mkdtemp(prefix="mirage-lock-regression-"))
    app = artifacts / "LockRegression.app"
    service = app / "Contents/XPCServices/LockRegressionService.xpc"
    bundle_id = "cn.laobamac.Mirage.LockRegression." + uuid.uuid4().hex
    for bundle, identifier, executable in [
        (app, bundle_id, "LockRegression"),
        (service, bundle_id + ".Service", "LockRegressionService"),
    ]:
        (bundle / "Contents/MacOS").mkdir(parents=True)
        info = dict(CFBundleIdentifier=identifier, CFBundleExecutable=executable,
                    CFBundleVersion="1", CFBundlePackageType="XPC!" if bundle == service else "APPL")
        if bundle == service:
            info["XPCService"] = dict(ServiceType="Application", RunLoopType="NSRunLoop", JoinExistingSession=True)
        else:
            info["LSUIElement"] = True
        (bundle / "Contents/Info.plist").write_bytes(plistlib.dumps(info))
    executable = app / "Contents/MacOS/LockRegression"
    sources = [
        "WallpaperExtensionSupport/MirageLockBridge.swift",
        "WallpaperLayout/WallpaperPosition.swift",
        "Mirage Wallpaper/Services/WallpaperExtensionRegistry.swift",
        "Mirage Wallpaper/Services/WallpaperExtensionController.swift",
        "Mirage Wallpaper Extension/LockConfigurationStore.swift",
        "Mirage Wallpaper Extension/CodableShims.swift",
        "Mirage Wallpaper Extension/SettingsProvider.swift",
        "Mirage Wallpaper Extension/SnapshotProvider.swift",
        "Tests/DynamicLockScreenRegression.swift",
    ]
    print(f"Artifacts: {artifacts}", flush=True)
    subprocess.run([
        "xcrun", "swiftc", "-parse-as-library", "-target", f"{platform.machine()}-apple-macos26.0",
        "-import-objc-header", str(project / "Mirage Wallpaper Extension/WallpaperExtension-Bridging-Header.h"),
        *[str(project / source) for source in sources], "-o", str(executable),
    ], check=True)
    shutil.copy2(executable, service / "Contents/MacOS/LockRegressionService")
    for bundle in [service, app]:
        subprocess.run(["codesign", "--force", "--sign", "-", str(bundle)], check=True)
    subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
