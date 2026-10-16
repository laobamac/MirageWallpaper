from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="mirage-desktop-restore-") as directory:
    executable = Path(directory) / "DesktopRestoreRegression"
    subprocess.run(["xcrun", "swiftc", "-parse-as-library", "-swift-version", "5",
                    "-module-cache-path", str(Path(tempfile.gettempdir()) / "MirageSafetyModuleCache"),
                    str(project / "Mirage Wallpaper/Services/DesktopRestoreOwnership.swift"),
                    str(project / "Tests/DesktopRestoreRegression.swift"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
