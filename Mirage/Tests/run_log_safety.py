from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="mirage-log-safety-") as directory:
    executable = Path(directory) / "LogSafetyRegression"
    subprocess.run(["xcrun", "swiftc", "-parse-as-library", "-swift-version", "5",
                    "-module-cache-path", str(Path(tempfile.gettempdir()) / "MirageSafetyModuleCache"),
                    str(project / "Mirage Wallpaper/Services/MirageLogService.swift"),
                    str(project / "Tests/LogSafetyRegression.swift"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=30)
