from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="mirage-pairing-") as directory:
    executable = Path(directory) / "MobilePairingRegression"
    subprocess.run(["xcrun", "swiftc", "-parse-as-library", "-swift-version", "5",
                    str(project / "Mirage Wallpaper/Services/MobilePairingAdmission.swift"),
                    str(project / "Mirage Wallpaper/Services/MobileSocketIO.swift"),
                    str(project / "Tests/MobilePairingRegression.swift"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
