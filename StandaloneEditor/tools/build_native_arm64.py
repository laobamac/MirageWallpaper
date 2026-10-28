#!/usr/bin/env python3
# Mirage Wallpaper
# Created on 2026/10/29. Copyright © 2026 王孝慈. All rights reserved.

"""Build native macOS ARM64 Wine and FEX from pinned, independently licensed sources."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import urllib.request

from runtime_layout import is_macho

PROJECT = Path(__file__).resolve().parents[1]
HADRON_COMMIT = '3e7043aa4264aab9d598ff6d52d49ac53d0fca00'
TOOLCHAIN_NAME = 'llvm-mingw-20260922-ucrt-macos-universal'
TOOLCHAIN_SHA256 = '52e5f5a7b131021d0c39a37a38fa380a1da7885cd04bd61afd0cd4ecfb8bc1f3'
SOURCES = {
    'wine': '6880117619afdf62b4ccc40a8c6268c86613131f',
    'fex': '0df84d3844bcdb87bb7d3f5b8fb0959cd009c038',
}


def run(*args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def output(*args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, default=PROJECT / 'build/native-arm64')
    parser.add_argument('--install-dependencies', action='store_true')
    args = parser.parse_args()
    if platform.machine() != 'arm64':
        parser.error('The native build must run on Apple Silicon, without Rosetta.')
    translated = subprocess.run(['sysctl', '-n', 'sysctl.proc_translated'], capture_output=True, text=True)
    if translated.stdout.strip() == '1':
        parser.error('The build process is translated. Start an ARM64 terminal.')
    if tuple(map(int, platform.mac_ver()[0].split('.')[:2])) < (26, 5):
        parser.error('The native Wine runtime requires macOS 26.5 or later.')
    brew = Path(output('brew', '--prefix'))
    if str(brew) != '/opt/homebrew':
        parser.error('Native ARM64 Homebrew is required at /opt/homebrew.')
    if args.install_dependencies:
        run('brew', 'install', 'llvm', 'lld', 'bison', 'flex', 'meson', 'ninja', 'cmake',
            'pkg-config', 'freetype', 'gnutls', 'molten-vk', 'vulkan-loader', 'vulkan-headers')

    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    upstream = work / 'hadron'
    if not (upstream / '.git').exists():
        run('git', 'clone', '--filter=blob:none', '--no-checkout',
            'https://github.com/Broyojo/hadron.git', upstream)
    run('git', '-C', upstream, 'fetch', 'origin', HADRON_COMMIT)
    run('git', '-C', upstream, 'checkout', '--detach', HADRON_COMMIT)
    if output('git', '-C', upstream, 'rev-parse', 'HEAD') != HADRON_COMMIT:
        raise RuntimeError('The native Wine patch set does not match the pinned revision.')
    declared = {parts[0]: parts[2] for line in (upstream / 'sources.conf').read_text().splitlines()
                if (parts := line.split()) and not parts[0].startswith('#')}
    for name, commit in SOURCES.items():
        if declared.get(name) != commit:
            raise RuntimeError('Unexpected upstream revision for ' + name)

    toolchains = upstream / 'toolchains'
    toolchains.mkdir(exist_ok=True)
    toolchain = toolchains / 'llvm-mingw'
    if not (toolchain / 'bin/arm64ec-w64-mingw32-clang').exists():
        archive = work / (TOOLCHAIN_NAME + '.tar.xz')
        if not archive.exists() or digest(archive) != TOOLCHAIN_SHA256:
            temporary = archive.with_suffix('.download')
            url = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/' + archive.name
            with urllib.request.urlopen(url, timeout=60) as response, temporary.open('wb') as stream:
                shutil.copyfileobj(response, stream)
            if digest(temporary) != TOOLCHAIN_SHA256:
                temporary.unlink()
                raise RuntimeError('The LLVM-MinGW archive failed SHA-256 verification.')
            temporary.replace(archive)
        with tarfile.open(archive) as bundle:
            bundle.extractall(toolchains, filter='data')
        (toolchains / TOOLCHAIN_NAME).replace(toolchain)
    # Target-specific clang commands are shell wrappers in LLVM-MinGW. Audit
    # their underlying executables, including linker and resource tools.
    native_tools = {p.resolve() for p in (toolchain / 'bin').iterdir() if is_macho(p.resolve())}
    if not native_tools:
        raise RuntimeError('The cross compiler toolchain contains no native executables.')
    for compiler in sorted(native_tools):
        if 'arm64' not in output('lipo', '-archs', compiler).split():
            raise RuntimeError('The cross compiler lacks a native ARM64 host slice: ' + str(compiler))

    environment = dict(os.environ, JOBS=os.environ.get('JOBS', '2'), HOMEBREW_NO_AUTO_UPDATE='1',
                       CMAKE_BUILD_PARALLEL_LEVEL=os.environ.get('JOBS', '2'))
    # Use Wine's official GitHub mirror for the same immutable source commit.
    environment.update(GIT_CONFIG_COUNT='1',
                       GIT_CONFIG_KEY_0='url.https://github.com/wine-mirror/wine.git.insteadOf',
                       GIT_CONFIG_VALUE_0='https://gitlab.winehq.org/wine/wine.git')
    run('bash', upstream / 'scripts/fetch.sh', 'wine', 'fex', env=environment)
    for name in SOURCES:
        run('git', '-C', upstream / 'src' / name, 'submodule', 'update', '--init', '--recursive',
            '--force', '--checkout', env=environment)
    # The high-address variant supports the x64 editor without a restricted low-address profile.
    run('bash', upstream / 'scripts/build-wine.sh', '--dev', env=environment)
    run('bash', upstream / 'scripts/build-fex.sh', '--dev', env=environment)
    runtime = upstream / 'dist-dev'
    for relative in ['bin/wine', 'bin/wineserver', 'lib/wine/aarch64-unix/ntdll.so',
                     'lib/wine/aarch64-unix/libarm64ecfex.so']:
        path = runtime / relative
        if not path.is_file() or output('lipo', '-archs', path) != 'arm64':
            raise RuntimeError('Native ARM64 runtime component is missing or mixed: ' + relative)
    emulator = runtime / 'lib/wine/aarch64-windows/xtajit64.dll'
    if not emulator.is_file() or emulator.stat().st_size < 1024 * 1024:
        raise RuntimeError('FEX ARM64EC was not installed; the Wine emulator stub is insufficient.')
    provenance = {
        'schema': 1, 'architecture': 'arm64', 'rosetta': False, 'minimumMacOS': '26.5',
        'hadron': HADRON_COMMIT, 'upstream': SOURCES, 'toolchainSHA256': TOOLCHAIN_SHA256,
        'patches': {str(p.relative_to(upstream)): digest(p)
                    for name in SOURCES for p in sorted((upstream / 'patches' / name).glob('*.patch'))},
    }
    (runtime / 'mirage-native-build.json').write_text(json.dumps(provenance, indent=2) + '\n')
    print('Native ARM64 Wine + FEX built:', runtime, flush=True)


if __name__ == '__main__':
    main()
