#!/usr/bin/env python3
"""vcpkg 资源下载适配：使用验证 TLS 的 requests，并核对 vcpkg 固定 SHA512。"""
from __future__ import annotations

import hashlib
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "build/tools/python"))
import requests


def main() -> None:
    url, expected, filename = sys.argv[1:]
    destination = Path(filename)
    if not destination.parent.is_dir():
        raise RuntimeError(f"下载父目录不存在: {destination.parent}")
    sources = [url]
    if url.startswith("https://mirror.msys2.org/"):
        sources.append(url.replace("https://mirror.msys2.org/", "https://mirror.selfnet.de/msys2/", 1))
    last_error = None
    for source in sources:
        for use_environment_proxy in (True, False):
            digest = hashlib.sha512()
            session = requests.Session()
            session.trust_env = use_environment_proxy
            try:
                with session:
                    with session.get(source, stream=True, timeout=(30, 180)) as response:
                        response.raise_for_status()
                        with destination.open("wb") as output:
                            for chunk in response.iter_content(1024 * 1024):
                                digest.update(chunk)
                                output.write(chunk)
            except requests.RequestException as error:
                last_error = error
                continue
            if digest.hexdigest() != expected.lower():
                raise RuntimeError(f"vcpkg SHA512 校验失败: {destination}")
            return
    if last_error is not None:
        raise last_error
    raise RuntimeError("vcpkg 资源下载失败")


if __name__ == "__main__":
    main()
