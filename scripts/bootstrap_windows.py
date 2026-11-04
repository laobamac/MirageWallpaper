#!/usr/bin/env python3
"""仅向工作区获取 Windows 构建工具，不安装系统组件。"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / "scripts/windows/dependencies.lock.json").read_text(encoding="utf-8"))
DEPS = ROOT / "build/deps"
TOOLS = ROOT / "build/tools"
CACHE = DEPS / "downloads"


def run(command: list[str], **kwargs) -> None:
    print(subprocess.list2cmdline([str(x) for x in command]), flush=True)
    subprocess.run([str(x) for x in command], check=True, **kwargs)


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def download(url: str, expected: str) -> Path:
    import requests
    CACHE.mkdir(parents=True, exist_ok=True)
    filename = url.rsplit("/", 1)[-1]
    target = CACHE / filename
    if target.is_file():
        if sha256(target) != expected:
            raise RuntimeError(f"缓存校验失败，请检查文件后手动移除: {target}")
        return target
    temporary = target.with_name(target.name + ".part")
    print(f"下载 {url}", flush=True)
    with requests.get(url, stream=True, timeout=(30, 180)) as response:
        response.raise_for_status()
        with temporary.open("wb") as stream:
            for chunk in response.iter_content(1024 * 1024):
                stream.write(chunk)
    actual = sha256(temporary)
    if actual != expected:
        raise RuntimeError(f"SHA256 不匹配: {temporary}: {actual} != {expected}")
    temporary.replace(target)
    return target


def python_packages() -> None:
    target = TOOLS / "python"
    target.mkdir(parents=True, exist_ok=True)
    run([sys.executable, "-m", "pip", "install", "--target", str(target),
         "--upgrade", *LOCK["python_packages"]])


def llvm() -> None:
    specification = LOCK["llvm"]
    target = TOOLS / "llvm"
    marker = target / ".mirage-version"
    if marker.is_file() and marker.read_text().strip() == specification["sha256"]:
        run([str(target / "bin/clang-cl.exe"), "--version"])
        return
    if target.exists() and any(target.iterdir()):
        raise RuntimeError(f"LLVM 目录非空但版本未知，请检查后指定新目录: {target}")
    archive = download(specification["url"], specification["sha256"])
    target.mkdir(parents=True, exist_ok=True)
    print(f"解包 LLVM 编译工具到 {target}", flush=True)
    with tarfile.open(archive, "r|xz") as bundle:
        for member in bundle:
            parts = Path(member.name).parts
            if len(parts) < 2:
                continue
            relative = Path(*parts[1:])
            # 编译需要所有 bin 工具与 Clang 资源头文件，不需要 LLVM 开发库。
            if parts[1] != "bin" and parts[1:3] != ("lib", "clang") and not parts[1].startswith("LICENSE"):
                continue
            entry = copy.copy(member)
            entry.name = relative.as_posix()
            bundle.extract(entry, target, filter="data")
    run([str(target / "bin/clang-cl.exe"), "--version"])
    marker.write_text(specification["sha256"] + "\n", encoding="ascii")


def vcpkg() -> None:
    specification = LOCK["vcpkg"]
    target = DEPS / "vcpkg"
    target.mkdir(parents=True, exist_ok=True)
    git = ["git", "-c", "http.sslBackend=openssl"]
    if not (target / ".git").exists():
        if any(target.iterdir()):
            raise RuntimeError(f"vcpkg 目录非空但不是 Git 工作树: {target}")
        run([*git, "init", str(target)])
        run([*git, "-C", str(target), "remote", "add", "origin", specification["repository"]])
        run([*git, "-C", str(target), "fetch", "--depth=1", "origin", specification["baseline"]])
        run([*git, "-C", str(target), "checkout", "--detach", specification["baseline"]])
    current = subprocess.check_output([*git, "-C", str(target), "rev-parse", "HEAD"], text=True).strip()
    if current != specification["baseline"]:
        raise RuntimeError(f"vcpkg baseline 不匹配: {current}")
    executable = target / "vcpkg.exe"
    archive = download(specification["tool_url"], specification["tool_sha256"])
    if executable.exists() and sha256(executable) != specification["tool_sha256"]:
        raise RuntimeError(f"已有 vcpkg.exe 版本不匹配: {executable}")
    if not executable.exists():
        shutil.copy2(archive, executable)
    run([str(executable), "version"])


def qt() -> None:
    specification = LOCK["qt"]
    target = DEPS / "Qt"
    prefix = target / specification["version"] / specification["directory"]
    modules = ("Qt6Core", "Qt6Svg", "Qt6WebChannel", "Qt6Positioning", "Qt6SerialPort", "Qt6WebEngineCore", "Qt6WebEngineWidgets", "Qt6WebEngineQuick")
    if (prefix / "bin/qmake.exe").exists() and all((prefix / "lib/cmake" / name / f"{name}Config.cmake").exists() for name in modules):
        return
    run([sys.executable, "-m", "aqt", "install-qt", "windows", "desktop",
         specification["version"], specification["arch"], "--outputdir", str(target),
         "--modules", *specification["modules"]])


def stage_mpv_licenses(target: Path) -> None:
    licenses = target / "Licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    sources = {
        "GPL-3.0.txt": ROOT / "SceneRenderer/Vendor/nlohmann_json/LICENSES/GPL-3.0-only.txt",
        "LGPL-2.1.txt": ROOT / "SteamService/Licenses/LGPL-2.1.txt",
        "Apache-2.0.txt": ROOT / "SceneRenderer/Vendor/nlohmann_json/LICENSES/Apache-2.0.txt",
    }
    for name, source in sources.items():
        shutil.copy2(source, licenses / name)
    shutil.copy2(ROOT / "scripts/windows/mpv-third-party-notices.txt", licenses / "THIRD-PARTY-NOTICES.txt")


def archive_dependency(name: str) -> None:
    specification = LOCK[name]
    target = DEPS / specification["directory"]
    marker = target / ".mirage-version"
    if name == "mpv":
        stage_mpv_licenses(target)
    if marker.is_file() and marker.read_text().strip() == specification["sha256"]:
        if name == "mpv":
            runtime = target / "libmpv-2.dll"
            if not runtime.is_file() or runtime.stat().st_size == 0:
                raise RuntimeError(f"mpv DLL 解包不完整: {runtime}")
        return
    archive = download(specification["url"], specification["sha256"])
    if "sha512" in specification:
        with archive.open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha512").hexdigest()
        if actual != specification["sha512"]:
            raise RuntimeError(f"SHA512 不匹配: {archive}")
    target.mkdir(parents=True, exist_ok=True)
    if specification.get("format", "zip") == "7z":
        extractor = TOOLS / "7zr.exe"
        tool = LOCK["sevenzip"]
        if extractor.exists() and sha256(extractor) != tool["sha256"]:
            raise RuntimeError(f"工作区 7zr 版本或校验不匹配: {extractor}")
        if not extractor.exists():
            shutil.copy2(download(tool["url"], tool["sha256"]), extractor)
        # 固定哈希的 mpv 官方构建包使用 BCJ2，必须调用原生 7zip 解码。
        run([str(extractor), "x", str(archive), f"-o{target}", "-y"])
        if name == "mpv":
            runtime = target / "libmpv-2.dll"
            if not runtime.is_file() or runtime.stat().st_size == 0:
                raise RuntimeError(f"mpv DLL 解包不完整: {runtime}")
    else:
        with zipfile.ZipFile(archive) as bundle:
            for item in bundle.infolist():
                if Path(item.filename).is_absolute() or ".." in Path(item.filename).parts:
                    raise RuntimeError("归档含不安全路径")
            bundle.extractall(target)
    marker.write_text(specification["sha256"] + "\n", encoding="ascii")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("components", nargs="*", choices=["python", "llvm", "vcpkg", "qt", "sdk", "dotnet", "mpv"])
    arguments = parser.parse_args()
    if os.name != "nt":
        parser.error("仅支持 Windows x64")
    TOOLS.mkdir(parents=True, exist_ok=True)
    DEPS.mkdir(parents=True, exist_ok=True)
    python_path = TOOLS / "python"
    sys.path.insert(0, str(python_path))
    os.environ["PYTHONPATH"] = os.pathsep.join([str(python_path), os.environ.get("PYTHONPATH", "")])
    for component in arguments.components or ["python", "llvm", "vcpkg", "qt", "sdk", "dotnet", "mpv"]:
        if component == "sdk":
            archive_dependency("sdk_cpp")
            archive_dependency("sdk_x64")
        elif component in ("dotnet", "mpv"):
            archive_dependency(component)
        else:
            globals()[component]() if component != "python" else python_packages()


if __name__ == "__main__":
    main()
