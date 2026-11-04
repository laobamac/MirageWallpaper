#!/usr/bin/env python3
"""使用工作区依赖构建并打包 Windows Qt 管理器、三种渲染器与 Steam 服务。"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / "scripts/windows/dependencies.lock.json").read_text(encoding="utf-8"))
DEPS = ROOT / "build/deps"
TOOLS = ROOT / "build/tools"
TRIPLET = "x64-mirage-windows"
COMPONENTS = {"scene": "SceneRenderer", "web": "WebRenderer", "video": "VideoRenderer", "app": "MirageQt"}
EXECUTABLES = {"scene": "SceneWallpaper.exe", "web": "WebWallpaper.exe", "video": "VideoWallpaper.exe", "app": "MirageQt.exe"}


def required(path: Path) -> Path:
    if not path.exists():
        raise RuntimeError(f"缺少依赖: {path}；先运行 scripts/bootstrap_windows.py")
    return path.resolve()


def run(command: list[str], environment: dict[str, str], **kwargs) -> None:
    command = [str(item) for item in command]
    print(subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, env=environment, check=True, **kwargs)


def msvc_root(explicit: str | None) -> Path:
    candidate = explicit or os.environ.get("VCToolsInstallDir")
    if candidate:
        return required(Path(candidate))
    installations = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Microsoft Visual Studio"
    candidates = list(installations.glob("*/**/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe"))
    if not candidates:
        raise RuntimeError("未找到 MSVC x64 编译器；使用 --msvc-root 指定 VC/Tools/MSVC/<版本>")
    return max(candidates, key=lambda p: tuple(int(x) for x in p.parents[3].name.split("."))).parents[3]


def debug_runtime_dirs(msvc: Path, sdk: Path, version: str) -> list[Path]:
    directories = [sdk / "bin" / version / "x64/ucrt"]
    redist = msvc.parents[3] / "VC/Redist/MSVC"
    candidates = [path for path in redist.glob("*/debug_nonredist/x64/Microsoft.VC*.DebugCRT")
                  if all(part.isdigit() for part in path.parents[2].name.split("."))]
    if candidates:
        directories.append(max(candidates, key=lambda p: tuple(int(x) for x in p.parents[2].name.split("."))))
    return [path for path in directories if path.is_dir()]


def build_environment(msvc: Path) -> dict[str, str]:
    sdk = required(DEPS / "microsoft.windows.sdk.cpp/c")
    libraries = required(DEPS / "microsoft.windows.sdk.cpp.x64/c")
    version = LOCK["windows_sdk"]["version"]
    includes = [msvc / "include", *[sdk / "Include" / version / name for name in ("ucrt", "shared", "um", "winrt", "cppwinrt")]]
    libs = [msvc / "lib/x64", libraries / "ucrt/x64", libraries / "um/x64"]
    for path in [*includes, *libs, msvc / "bin/Hostx64/x64/cl.exe", sdk / "bin" / version / "x64/rc.exe"]:
        required(path)
    environment = dict(os.environ)
    environment.update({
        "INCLUDE": os.pathsep.join(str(p) for p in includes),
        "LIB": os.pathsep.join(str(p) for p in libs),
        "LIBPATH": str(msvc / "lib/x64"),
        "VCToolsInstallDir": msvc.as_posix() + "/",
        "VCToolsVersion": msvc.name,
        "VCINSTALLDIR": msvc.parents[2].as_posix() + "/",
        "VSINSTALLDIR": msvc.parents[3].as_posix() + "/",
        "WindowsSdkDir": sdk.as_posix() + "/",
        "WindowsSDKVersion": version + "/",
        "UniversalCRTSdkDir": sdk.as_posix() + "/",
        "UCRTVersion": version,
        "VCPKG_ROOT": str(DEPS / "vcpkg"),
        "VCPKG_DISABLE_METRICS": "1",
        "VCPKG_DOWNLOADS": str(DEPS / "vcpkg/downloads"),
        "VCPKG_BINARY_SOURCES": f"clear;files,{DEPS / 'vcpkg/binary-cache'},readwrite",
        "NUGET_PACKAGES": str(DEPS / "nuget"),
        "DOTNET_CLI_HOME": str(DEPS / "dotnet-home"),
        "DOTNET_CLI_TELEMETRY_OPTOUT": "1",
        "DOTNET_NOLOGO": "1",
        "DOTNET_MULTILEVEL_LOOKUP": "0",
        "PYTHONPATH": os.pathsep.join([str(TOOLS / "python"), environment.get("PYTHONPATH", "")]),
    })
    qt = DEPS / "Qt" / LOCK["qt"]["version"] / LOCK["qt"]["directory"]
    installed = ROOT / "build/windows/vcpkg_installed" / TRIPLET
    paths = [msvc / "bin/Hostx64/x64", sdk / "bin" / version / "x64", TOOLS / "python/cmake/data/bin", TOOLS / "python/bin", DEPS / "dotnet10", TOOLS / "llvm/bin", qt / "bin", DEPS / "mpv", installed / "bin", installed / "debug/bin", *debug_runtime_dirs(msvc, sdk, version)]
    environment["PATH"] = os.pathsep.join([*[str(p) for p in paths], environment.get("PATH", "")])
    downloader = subprocess.list2cmdline([sys.executable, str(ROOT / "scripts/windows/vcpkg_download.py")])
    downloader = downloader.replace("`", "``").replace(";", "`;").replace(",", "`,")
    environment["X_VCPKG_ASSET_SOURCES"] = f"clear;x-script,{downloader} {{url}} {{sha512}} {{dst}};x-block-origin"
    if (DEPS / "dotnet10/dotnet.exe").exists():
        environment["DOTNET_ROOT"] = str(DEPS / "dotnet10")
    return environment


def mpv_import_library(environment: dict[str, str]) -> Path:
    for name in ("libmpv.lib", "mpv.lib", "mpv-2.lib"):
        for directory in (DEPS / "mpv", DEPS / "mpv/lib"):
            if (directory / name).is_file():
                return directory / name
    runtime = required(DEPS / "mpv/libmpv-2.dll")
    target = ROOT / "build/windows/mpv"
    target.mkdir(parents=True, exist_ok=True)
    library = target / "libmpv.lib"
    if library.is_file() and library.stat().st_mtime >= runtime.stat().st_mtime:
        return library
    binaries = Path(environment["VCToolsInstallDir"]) / "bin/Hostx64/x64"
    output = subprocess.check_output([str(binaries / "dumpbin.exe"), "/EXPORTS", str(runtime)], env=environment, text=True, errors="replace")
    exports = re.findall(r"^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(mpv_\w+)\s*$", output, re.MULTILINE)
    if not exports:
        raise RuntimeError("libmpv DLL 未包含 mpv API 导出")
    definition = target / "libmpv.def"
    definition.write_text("LIBRARY libmpv-2.dll\nEXPORTS\n" + "\n".join(exports) + "\n", encoding="ascii")
    run([str(binaries / "lib.exe"), "/NOLOGO", "/MACHINE:X64", f"/DEF:{definition}", f"/OUT:{library}"], environment)
    return required(library)


def cmake_options(configuration: str, environment: dict[str, str], component: str, destination: Path, tests: bool) -> list[str]:
    sdk_bin = Path(environment["WindowsSdkDir"]) / "bin" / LOCK["windows_sdk"]["version"] / "x64"
    msvc_bin = Path(environment["VCToolsInstallDir"]) / "bin/Hostx64/x64"
    compiler = TOOLS / "llvm/bin/clang-cl.exe" if component == "scene" else msvc_bin / "cl.exe"
    required(compiler)
    qt = DEPS / "Qt" / LOCK["qt"]["version"] / LOCK["qt"]["directory"]
    if component != "scene":
        required(qt / "lib/cmake/Qt6/Qt6Config.cmake")
    options = [
        "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={configuration}",
        f"-DCMAKE_MAKE_PROGRAM={required(TOOLS / 'python/bin/ninja.exe').as_posix()}",
        f"-DCMAKE_CXX_COMPILER={compiler.as_posix()}",
        f"-DCMAKE_RC_COMPILER={(sdk_bin / 'rc.exe').as_posix()}", f"-DCMAKE_MT={(sdk_bin / 'mt.exe').as_posix()}",
        f"-DCMAKE_LINKER={(msvc_bin / 'link.exe').as_posix()}",
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL",
        f"-DCMAKE_INSTALL_PREFIX={destination.as_posix()}", "-DCMAKE_INSTALL_BINDIR=.",
        f"-DCMAKE_PREFIX_PATH={qt.as_posix()}",
    ]
    if component == "scene":
        installed = ROOT / "build/windows/vcpkg_installed" / TRIPLET
        options.extend([
            f"-DCMAKE_C_COMPILER={compiler.as_posix()}",
            f"-DCMAKE_PREFIX_PATH={installed.as_posix()}",
            f"-DCMAKE_TOOLCHAIN_FILE={(DEPS / 'vcpkg/scripts/buildsystems/vcpkg.cmake').as_posix()}",
            f"-DVCPKG_TARGET_TRIPLET={TRIPLET}", f"-DVCPKG_HOST_TRIPLET={TRIPLET}",
            f"-DVCPKG_OVERLAY_TRIPLETS={(ROOT / 'scripts/windows').as_posix()}",
            f"-DVCPKG_OVERLAY_PORTS={(ROOT / 'scripts/windows/ports').as_posix()}",
            f"-DVCPKG_INSTALLED_DIR={(ROOT / 'build/windows/vcpkg_installed').as_posix()}",
            "-DVCPKG_MANIFEST_INSTALL=OFF", "-DSCENERENDERER_BUILD_VIEWER=OFF",
            f"-DSCENERENDERER_BUILD_TESTS={'ON' if tests else 'OFF'}",
        ])
    elif component == "web":
        options.extend(["-DWEBRENDERER_BUILD_VIEWER=OFF", f"-DWEBRENDERER_BUILD_TESTS={'ON' if tests else 'OFF'}"])
    elif component == "video":
        options.extend([f"-DMPV_ROOT={(DEPS / 'mpv').as_posix()}",
                        f"-DMPV_LIBRARY={mpv_import_library(environment).as_posix()}",
                        f"-DVIDEORENDERER_BUILD_TESTS={'ON' if tests else 'OFF'}"])
    elif component == "app":
        options.extend([f"-DMIRAGEQT_RUNTIME_DIR={destination.as_posix()}", f"-DMIRAGEQT_DATA_DIR={destination.as_posix()}"])
    return options


def install_scene_dependencies(environment: dict[str, str]) -> None:
    executable = required(DEPS / "vcpkg/vcpkg.exe")
    for path in [DEPS / "vcpkg/binary-cache", ROOT / "build/windows"]:
        path.mkdir(parents=True, exist_ok=True)
    baseline = subprocess.check_output(["git", "-C", str(DEPS / "vcpkg"), "rev-parse", "HEAD"], text=True).strip()
    if baseline != LOCK["vcpkg"]["baseline"]:
        raise RuntimeError(f"vcpkg baseline 不匹配: {baseline}")
    run([str(executable), "install", f"--triplet={TRIPLET}",
         f"--host-triplet={TRIPLET}", f"--overlay-triplets={ROOT / 'scripts/windows'}",
         f"--overlay-ports={ROOT / 'scripts/windows/ports'}",
         f"--x-manifest-root={ROOT / 'scripts/windows'}",
         f"--x-install-root={ROOT / 'build/windows/vcpkg_installed'}", "--disable-metrics"], environment)


def deploy(destination: Path, environment: dict[str, str], configuration: str) -> None:
    qt = DEPS / "Qt" / LOCK["qt"]["version"] / LOCK["qt"]["directory"]
    executables = [destination / filename for filename in EXECUTABLES.values() if (destination / filename).is_file()]
    qt_executables = [path for path in executables if path.name != "SceneWallpaper.exe"]
    if qt_executables:
        deploy_tool = required(qt / "bin/windeployqt.exe")
        run([str(deploy_tool), "--debug" if configuration == "Debug" else "--release",
             "--dir", str(destination), "--qmldir", str(ROOT / "MirageQt/Sources/Qml"),
             "--compiler-runtime", *[str(p) for p in qt_executables]], environment)
    if configuration == "Debug":
        msvc = Path(environment["VCToolsInstallDir"])
        sdk = Path(environment["WindowsSdkDir"])
        version = LOCK["windows_sdk"]["version"]
        required(sdk / "bin" / version / "x64/ucrt/ucrtbased.dll")
        for directory in debug_runtime_dirs(msvc, sdk, version):
            for dll in directory.glob("*.dll"):
                shutil.copy2(dll, destination / dll.name)
    license_target = destination / "Licenses"
    license_target.mkdir(parents=True, exist_ok=True)
    runtime = DEPS / "mpv/libmpv-2.dll"
    if (destination / "VideoWallpaper.exe").exists():
        shutil.copy2(required(runtime), destination / runtime.name)
        shutil.copytree(required(DEPS / "mpv/Licenses"), license_target / "mpv", dirs_exist_ok=True)
    if (destination / "SceneWallpaper.exe").exists():
        asset_target = destination / "assets"
        shutil.copytree(ROOT / "assets", asset_target, dirs_exist_ok=True)
    shutil.copy2(ROOT / "LICENSE", license_target / "Mirage-LICENSE.txt")
    installed = ROOT / "build/windows/vcpkg_installed" / TRIPLET
    if installed.exists():
        runtime_dir = installed / ("debug/bin" if configuration == "Debug" else "bin")
        for dll in runtime_dir.glob("*.dll"):
            shutil.copy2(dll, destination / dll.name)
        for license_file in installed.glob("share/*/copyright"):
            target = license_target / "vcpkg" / license_file.parent.name
            target.mkdir(parents=True, exist_ok=True)
            shutil.copy2(license_file, target / "copyright")
    if (qt / "LICENSES").exists():
        shutil.copytree(qt / "LICENSES", license_target / "Qt", dirs_exist_ok=True)
    shutil.copy2(ROOT / "scripts/windows/dependencies.lock.json", destination / "dependencies.lock.json")


def publish_steam(destination: Path, environment: dict[str, str], configuration: str) -> None:
    executable = required(DEPS / "dotnet10/dotnet.exe")
    target = destination / "SteamService/win-x64"
    run([str(executable), "publish", str(ROOT / "SteamService/MirageSteamService.csproj"),
         "--configuration", configuration, "--runtime", "win-x64", "--self-contained", "true",
         "-p:RestoreLockedMode=true", "--output", str(target)], environment)
    license_target = destination / "SteamService/Licenses"
    shutil.copytree(ROOT / "SteamService/Licenses", license_target, dirs_exist_ok=True)
    for filename in ("LICENSE.txt", "ThirdPartyNotices.txt"):
        source = DEPS / "dotnet10" / filename
        if source.is_file():
            shutil.copy2(source, license_target / f"dotnet-{filename}")


def verify(destination: Path, configuration: str = "Release") -> None:
    suffix = "d" if configuration == "Debug" else ""
    expected = [*EXECUTABLES.values(), f"platforms/qwindows{suffix}.dll", f"Qt6WebEngineCore{suffix}.dll",
                f"QtWebEngineProcess{suffix}.exe", "resources/icudtl.dat", "libmpv-2.dll", "vulkan-1.dll",
                "SteamService/win-x64/MirageSteamService.exe", "SteamService/win-x64/SteamKit2.dll",
                "Licenses/mpv/GPL-3.0.txt", "Licenses/mpv/LGPL-2.1.txt",
                "Licenses/mpv/Apache-2.0.txt", "Licenses/mpv/THIRD-PARTY-NOTICES.txt",
                "assets/fonts", "dependencies.lock.json"]
    missing = [name for name in expected if not (destination / name).exists()]
    if missing:
        raise RuntimeError("部署文件不完整: " + ", ".join(missing))
    status = "（文件存在性检查；libmpv 许可审计未完成，ZIP 分发阻断）" if (destination / EXECUTABLES["video"]).is_file() else ""
    print(f"部署文件检查通过{status}: {destination}", flush=True)


def package(destination: Path) -> Path:
    if (destination / EXECUTABLES["video"]).is_file():
        raise RuntimeError(
            "Windows distribution is blocked: the pinned libmpv build lacks a complete, "
            "version-specific third-party license and corresponding-source audit. "
            "See Licenses/mpv/THIRD-PARTY-NOTICES.txt."
        )
    manifest = {path.relative_to(destination).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(destination.rglob("*")) if path.is_file() and path.name != "files.sha256.json"}
    (destination / "files.sha256.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    archive = destination.parent / f"{destination.name}.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as output:
        for path in sorted(destination.rglob("*")):
            if path.is_file():
                output.write(path, Path(destination.name) / path.relative_to(destination))
    checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(".zip.sha256").write_text(f"{checksum}  {archive.name}\n", encoding="ascii")
    return archive


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--components", nargs="+", choices=[*COMPONENTS, "steam"], default=[*COMPONENTS, "steam"])
    parser.add_argument("--config", choices=["Release", "Debug", "RelWithDebInfo"], default="Release")
    parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    parser.add_argument("--msvc-root")
    parser.add_argument("--output", type=Path, default=ROOT / "dist/Mirage-Windows-x64")
    parser.add_argument("--skip-dependencies", action="store_true")
    parser.add_argument("--dependencies-only", action="store_true")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--no-deploy", action="store_true")
    parser.add_argument("--tests", action="store_true")
    parser.add_argument("--package", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--print-environment", action="store_true")
    arguments = parser.parse_args()
    if os.name != "nt":
        parser.error("仅支持 Windows x64")
    destination = arguments.output.resolve()
    if arguments.verify_only:
        verify(destination, arguments.config)
        return
    environment = build_environment(msvc_root(arguments.msvc_root))
    if arguments.print_environment:
        keys = ["PATH", "INCLUDE", "LIB", "LIBPATH", "VCToolsInstallDir", "WindowsSdkDir", "WindowsSDKVersion", "NUGET_PACKAGES", "DOTNET_ROOT"]
        print(json.dumps({key: environment.get(key, "") for key in keys}, ensure_ascii=False, indent=2))
        return
    cmake = required(TOOLS / "python/cmake/data/bin/cmake.exe")
    if "scene" in arguments.components and not arguments.skip_dependencies:
        install_scene_dependencies(environment)
    if arguments.dependencies_only:
        return
    destination.mkdir(parents=True, exist_ok=True)
    for component in arguments.components:
        if component == "steam":
            if not arguments.configure_only:
                publish_steam(destination, environment, arguments.config)
            continue
        build = ROOT / "build/windows" / arguments.config / component
        options = cmake_options(arguments.config, environment, component, destination, arguments.tests)
        run([str(cmake), "-S", str(ROOT / COMPONENTS[component]), "-B", str(build), *options], environment)
        if arguments.configure_only:
            continue
        run([str(cmake), "--build", str(build), "--parallel", str(arguments.jobs)], environment)
        if arguments.tests:
            ctest = cmake.with_name("ctest.exe")
            command = [str(ctest), "--test-dir", str(build), "--output-on-failure"]
            if component == "scene":
                # Vulkan frame capture requires a hardware-capable interactive Windows host.
                command.extend(["--exclude-regex", "SceneRendererWallpaperPosition"])
            run(command, environment)
        run([str(cmake), "--install", str(build)], environment)
    if not arguments.configure_only and not arguments.no_deploy:
        deploy(destination, environment, arguments.config)
        if set(arguments.components) == {*COMPONENTS, "steam"}:
            verify(destination, arguments.config)
        if arguments.package:
            print(f"ZIP 包: {package(destination)}", flush=True)


if __name__ == "__main__":
    main()
