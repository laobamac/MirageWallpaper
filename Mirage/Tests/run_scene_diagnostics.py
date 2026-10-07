__copyright__ = "Copyright © 2026 王孝慈. All rights reserved."

from pathlib import Path
import argparse
import importlib.util
import json
import os
import platform
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True


def native_capture(project, libraries, wallpaper, production=False):
    root = Path(tempfile.mkdtemp(prefix="mirage-native-color-diagnostics-"))
    (root / "runtime.json").write_text(json.dumps(dict(speed=1, scriptStorage={})))
    print(f"Native capture output: {root}", flush=True)
    variants = [("production", False), ("production", True)] if production else [("baseline", False), ("patched", False), ("baseline", True)]
    for variant, metalfx in variants:
        name = variant + ("-metalfx" if metalfx else "")
        out = root / name
        out.mkdir()
        cache = root / (name + "-cache")
        cache.mkdir()
        icd = cache / "icd.json"
        icd.write_text(json.dumps(dict(file_format_version="1.0.0", ICD=dict(
            library_path=str(libraries.resolve() / "libMoltenVK.dylib" if production else libraries.resolve() / variant / "libMoltenVK.dylib"),
            api_version="1.4.0", is_portability_driver=True))))
        env = os.environ.copy()
        env.update(RSTD_LOG="info", MVK_CONFIG_FAST_MATH_ENABLED="0", VK_DRIVER_FILES=str(icd), VK_ICD_FILENAMES=str(icd),
                   SCENERENDERER_DIAGNOSTICS_DIR=str(out), SCENERENDERER_DIAGNOSTIC_CACHE=str(cache / "pipeline"),
                   SCENERENDERER_DUMP_FRAME=str(out / "frame.ppm"), SCENERENDERER_DUMP_FRAME_AT="0",
                   SCENERENDERER_DUMP_PRESENT=str(out / "present.ppm"))
        if production:
            env.pop("MVK_CONFIG_FAST_MATH_ENABLED", None)
        command = [str(project.parent / "SceneRenderer/build/macos-clang-release/Tools/SceneWallpaper/SceneWallpaper"),
                   str(project.parent / "assets"), str(wallpaper.resolve()), "--fps", "30", "--resolution", "800x520",
                   "--muted", "--external-spectrum", "--runtime", str(root / "runtime.json"), "--cache-path", str(cache), "--run-seconds", "110"]
        if metalfx:
            command.append("--metalfx")
        with (out / "renderer.log").open("w") as log:
            process = subprocess.Popen(command, env=env, cwd=cache, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 120
                while process.poll() is None and time.monotonic() < deadline and not (out / "ready").exists():
                    time.sleep(0.2)
                assert (out / "ready").exists(), f"Renderer did not become ready: {out}"
                assert not (out / "frame.ppm").exists(), "Readback ran before live observation"
                assert not list(out.glob("stage-*.ppm")), "Stage readback ran before live observation"
                (out / "capture").write_bytes(b"")
                mode = json.loads((out / "presentation.json").read_text())
                deadline = time.monotonic() + 20
                while process.poll() is None and time.monotonic() < deadline:
                    if (out / "captured").exists() and (not mode["metalfx_presenter"] or (out / "metalfx-present.ppm").exists()):
                        break
                    time.sleep(0.1)
                assert (out / "captured").exists(), f"Capture incomplete: {out}"
                assert (out / "frame.ppm").stat().st_size > 1000
                assert list(out.glob("stage-*.json")), "No stage diagnostics recorded"
                if mode["metalfx_presenter"]:
                    assert (out / "metalfx-present.ppm").stat().st_size > 1000
                print(f"{name}: live observation precedes capture; stages and final output recorded; {mode}", flush=True)
            finally:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--production", action="store_true")
    parser.add_argument("--libraries", type=Path)
    parser.add_argument("--wallpaper", type=Path)
    args = parser.parse_args()
    if args.native and (args.libraries is None or args.wallpaper is None):
        parser.error("--native requires --libraries and --wallpaper")
    project = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("diagnostic_builder", project / "scripts/build_moltenvk.py")
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    fixture = "prefix\n" + builder.ORIGINAL + "\nsuffix"
    patched = builder.patch_source(fixture)
    assert patched == "prefix\n" + builder.PATCHED + "\nsuffix"
    for wrong in ["", builder.ORIGINAL * 2, builder.PATCHED]:
        try:
            builder.patch_source(wrong)
            raise AssertionError("Unexpected source accepted")
        except ValueError:
            pass
    for language in ("en", "zh-Hans", "zh-Hant"):
        path = project / f"Mirage Wallpaper/Resources/{language}.lproj/Localizable.strings"
        parsed = json.loads(subprocess.check_output(["plutil", "-convert", "json", "-o", "-", str(path)]))
        assert "场景颜色诊断" in parsed
        assert "诊断：受控基线" in parsed
        assert "导出诊断报告…" in parsed
    with tempfile.TemporaryDirectory(prefix="mirage-diagnostics-tests-") as temporary:
        binary = Path(temporary) / "SceneDiagnosticsRegression"
        subprocess.run([
            "xcrun", "swiftc", "-swift-version", "5", "-parse-as-library", "-target", f"{platform.machine()}-apple-macos14.2",
            str(project / "Mirage Wallpaper/Services/SceneColorDiagnostics.swift"),
            str(project / "Tests/SceneDiagnosticRegression.swift"), "-o", str(binary),
        ], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=45)
    print("Build-source guards and three localization catalogs passed", flush=True)
    if args.native:
        native_capture(project, args.libraries, args.wallpaper, args.production)


if __name__ == "__main__":
    main()
