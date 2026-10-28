#!/usr/bin/env python3
# Copyright © 2026 王孝慈. All rights reserved.
"""Package native ARM64 Wine/FEX and Mirage adapters without private WE content."""

import argparse
import json
import os
from pathlib import Path
import plistlib
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile

from build_icon import build_icon
from build_native_arm64 import HADRON_COMMIT, SOURCES
from build_runtime import AUDIO_USAGE
from runtime_layout import audit, digest, make_portable, output, thin_runtime

PROJECT = Path(__file__).resolve().parents[1]
SOURCE = PROJECT / 'src'
DXVK_ARCHIVE_SHA = '5644f5c02e8dc3e25171e6b7b5d16e927332b32136c6caf8e418e1192cc2e5d4'
DXVK_DLL_SHA = '0c7db37a8baa1026736d643c8870587af3449cc366404e0bd18c685beb382cd8'
AUDIO_ABI = {
    'dlls/winecoreaudio.drv/coreaudio.c': 'db0b96d80e4bbca10e6b3b55630eda817716d7221960c585e12298001833e92a',
    'dlls/mmdevapi/unixlib.h': '0de9fde40d091d82afb74df6d01628b87044fc4e0976289af8872919cea2fc21',
}


def run(*args):
    subprocess.run(list(map(str, args)), check=True)


def bundle_app(path, identifier, name, executable, icon, accessory=False):
    contents = path / 'Contents'
    (contents / 'MacOS').mkdir(parents=True)
    resources = contents / 'Resources'
    resources.mkdir()
    shutil.copy2(icon, resources / 'MirageEditor.icns')
    info = dict(CFBundleIdentifier=identifier, CFBundleName=name, CFBundleDisplayName=name,
                CFBundleExecutable=executable, CFBundlePackageType='APPL', CFBundleVersion='4',
                CFBundleIconFile='MirageEditor.icns', CFBundleDevelopmentRegion='en',
                CFBundleLocalizations=list(AUDIO_USAGE), NSHighResolutionCapable=True,
                LSMinimumSystemVersion='26.5', NSAudioCaptureUsageDescription=AUDIO_USAGE['en'],
                NSHumanReadableCopyright='Copyright © 2026 王孝慈. All rights reserved.')
    if accessory:
        info['LSUIElement'] = True
    (contents / 'Info.plist').write_bytes(plistlib.dumps(info))
    for language, usage in AUDIO_USAGE.items():
        locale = resources / (language + '.lproj')
        locale.mkdir()
        (locale / 'InfoPlist.strings').write_text('"NSAudioCaptureUsageDescription" = ' + json.dumps(usage, ensure_ascii=False) + ';\n')


def native_audio_uuid(upstream, wine):
    # Validate both the function-table order and the request layout before
    # allowing this build's UUID to activate the native audio hook.
    for relative, expected in AUDIO_ABI.items():
        if digest(upstream / 'src/wine' / relative) != expected:
            raise ValueError('Unsupported native Wine audio ABI: ' + relative)
    binary = wine / 'lib/wine/aarch64-unix/winecoreaudio.so'
    if output('lipo', '-archs', binary) != 'arm64':
        raise ValueError('The native audio driver is not ARM64.')
    uuid = re.search(r'UUID: ([0-9A-Fa-f-]{36}) \(arm64\)', output('dwarfdump', '--uuid', binary))
    if not uuid:
        raise ValueError('Cannot identify the native audio driver.')
    return ','.join('0x' + format(byte, '02x') for byte in bytes.fromhex(uuid[1].replace('-', '')))


def prepare(upstream, destination):
    if platform.machine() != 'arm64':
        raise ValueError('Package the native runtime on Apple Silicon using ARM64 dependencies.')
    upstream = upstream.resolve()
    wine = upstream / 'dist-dev'
    provenance = json.loads((wine / 'mirage-native-build.json').read_text())
    if provenance.get('hadron') != HADRON_COMMIT or provenance.get('upstream') != SOURCES or provenance.get('rosetta') is not False:
        raise ValueError('Build the pinned native Wine/FEX runtime first.')
    audio_uuid = native_audio_uuid(upstream, wine)
    brew = Path(output('brew', '--prefix'))
    compiler = upstream / 'toolchains/llvm-mingw/bin/x86_64-w64-mingw32-clang'
    headers = brew / 'opt/vulkan-headers/include'
    archive = upstream.parent / 'dxvk-v1.10.3.tar.gz'
    if not archive.exists() or digest(archive) != DXVK_ARCHIVE_SHA:
        run('curl', '--fail', '--location', '--retry', '3',
            'https://github.com/Gcenx/DXVK-macOS/releases/download/v1.10.3/dxvk-v1.10.3.tar.gz', '-o', archive)
    if digest(archive) != DXVK_ARCHIVE_SHA:
        raise ValueError('DXVK archive failed SHA-256 verification.')
    destination = destination.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='editor-arm64-', dir=destination.parent) as temporary:
        package = Path(temporary)
        runtime = package / 'runtime'
        runtime.mkdir()
        icon = runtime / 'MirageEditor.icns'
        build_icon(icon)
        launcher = package / 'Mirage Editor.app'
        editor = runtime / 'Mirage WE Editor.app'
        picker = runtime / 'Mirage Project Picker.app'
        bundle_app(launcher, 'cn.laobamac.Mirage.EditorLauncher', 'Mirage Editor', 'Launcher', icon, True)
        bundle_app(editor, 'cn.laobamac.Mirage.WEEditor.Editor', 'Mirage Editor', 'wine', icon)
        bundle_app(picker, 'cn.laobamac.Mirage.WEEditor.ProjectPicker', 'Mirage Project Picker', 'ProjectPicker', icon, True)
        engine = editor / 'Contents/Resources/wine'
        shutil.copytree(wine, engine, symlinks=True, ignore=shutil.ignore_patterns('*.a', '*.pdb', '*.dSYM', '.DS_Store'))
        for relative in ['include', 'share/man', 'share/aclocal', 'lib/pkgconfig']:
            path = engine / relative
            if path.exists():
                shutil.rmtree(path)
        # Keep the public loader entries regular files: the installation catalog
        # verifies their signed bytes independently from the internal links.
        for target in [engine / 'bin/wine', editor / 'Contents/MacOS/wine']:
            if target.exists() or target.is_symlink():
                target.unlink()
            shutil.copy2(wine / 'bin/wine', target)
        (editor / 'Contents/MacOS/ntdll.so').symlink_to('../Resources/wine/lib/wine/aarch64-unix/ntdll.so')
        configuration = (upstream / 'build/wine-dev/include/config.h').read_text()
        by_name = []
        for library in ['FREETYPE', 'GNUTLS', 'SDL2', 'VULKAN']:
            match = re.search(r'^#define SONAME_LIB' + library + r' "([^"]+)"', configuration, re.M)
            if match:
                dependency = Path(match[1])
                by_name.append(dependency if dependency.is_absolute() else brew / 'lib' / dependency)
        # Wine dlopens these SONAMEs; provide the private library directory at
        # launch in addition to relocating direct Mach-O load commands.
        molten = brew / 'opt/molten-vk/lib/libMoltenVK.dylib'
        dependencies = make_portable(engine, by_name + [molten])
        icd = engine / 'lib/wine/aarch64-unix/vulkan/icd.d/MoltenVK_icd.json'
        icd.parent.mkdir(parents=True, exist_ok=True)
        molten_target = engine / 'lib/external' / molten.resolve().name
        icd.write_text(json.dumps({'file_format_version': '1.0.0', 'ICD': {
            'library_path': os.path.relpath(molten_target, icd.parent), 'api_version': '1.2.0'}}) + '\n')
        # The identity loader was copied before relocation. Match its rewritten
        # dependencies to the relocated library tree as well.
        make_portable(editor / 'Contents')
        for source, target in [('Launcher.swift', launcher / 'Contents/MacOS/Launcher'),
                               ('NativeBridge.swift', picker / 'Contents/MacOS/ProjectPicker')]:
            run('swiftc', '-target', 'arm64-apple-macos26.5', '-D', 'MIRAGE_MANAGED_EDITOR', SOURCE / source, '-o', target)
        run('clang', '-arch', 'arm64', '-mmacosx-version-min=26.5', '-O2', '-dynamiclib', '-fobjc-arc',
            '-I' + str(headers), '-DMIRAGE_AUDIO_UUID_BYTES=' + audio_uuid,
            '-framework', 'AppKit', '-framework', 'QuartzCore', '-framework', 'Metal', '-framework', 'CoreAudio',
            *[SOURCE / name for name in ['NativeCompositor.m', 'NativeIdentity.m', 'MetalInterop.m', 'ShaderOptimizer.m', 'SystemAudio.m']],
            '-o', runtime / 'MetalInterop.dylib')
        overrides = runtime / 'overrides'
        overrides.mkdir()
        with tarfile.open(archive) as bundle:
            for original, name in [('d3d11.dll', 'mirage_dxvk.dll'), ('d3d10core.dll', 'd3d10core.dll')]:
                (overrides / name).write_bytes(bundle.extractfile('dxvk-v1.10.3/x64/' + original).read())
        if digest(overrides / 'mirage_dxvk.dll') != DXVK_DLL_SHA:
            raise ValueError('Unexpected DXVK D3D11 ABI.')
        run(compiler, '-shared', '-O2', '-I' + str(headers), SOURCE / 'D3D11MetalInterop.c',
            SOURCE / 'd3d11.def', '-o', overrides / 'd3d11.dll', '-ldxguid')
        for name in ['standalone.js', 'macos.css', 'dxvk.conf', 'standalone-defaults.json']:
            shutil.copy2(SOURCE / name, runtime / name)
        adaptations = json.loads((SOURCE / 'EditorAdaptations.json').read_text())['patches']
        (runtime / 'adaptations.json').write_text(json.dumps(adaptations, separators=(',', ':')))
        shutil.copytree(PROJECT / 'Licenses', package / 'Licenses')
        for component in ['wine', 'fex']:
            source = upstream / 'src' / component
            for path in source.rglob('*'):
                if path.is_file() and not path.is_symlink() and path.name.upper().startswith(('LICENSE', 'COPYING')) and '.git' not in path.parts:
                    target = package / 'Licenses' / component / path.relative_to(source)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(path, target)
        shutil.copy2(upstream / 'LICENSE.hadron', package / 'Licenses/Hadron-LICENSE.txt')
        shutil.copytree(upstream / 'patches/wine', package / 'Licenses/wine-patches')
        shutil.copytree(upstream / 'patches/fex', package / 'Licenses/fex-patches')
        # Retain Homebrew notices for each relocated dependency and its source version.
        for dependency in dependencies.values():
            match = re.search(r'(/(?:opt/homebrew|usr/local)/Cellar/[^/]+/[^/]+)/', str(dependency))
            if match:
                keg = Path(match[1])
                for license_file in keg.rglob('*'):
                    if license_file.is_file() and not license_file.is_symlink() and license_file.name.upper().startswith(('LICENSE', 'COPYING')):
                        target = package / 'Licenses/Homebrew' / keg.parent.name / license_file.relative_to(keg)
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(license_file, target)
        provenance.update(dxvkSHA256=DXVK_DLL_SHA, moltenVKSHA256=digest(molten), audioABI=AUDIO_ABI)
        (runtime / 'native-build.json').write_text(json.dumps(provenance, indent=2) + '\n')
        (package / 'LICENSE-NOTICE.txt').write_text(
            'Mirage Editor adapters: Copyright © 2026 王孝慈. All rights reserved.\n'
            'No Wallpaper Engine files or initialized Wine prefix are included.\n'
            'Native ARM64 Wine + FEX; original Windows x64 instructions execute through FEX, without Rosetta.\n'
            'Third-party licenses and Wine/FEX patches are in Licenses/. Source revisions are in runtime/native-build.json.\n')
        thin_runtime(package, 'arm64')
        audit(package, 'arm64')
        catalog = dict(schema=1, architecture='arm64', minimumMacOS='26.5', rosetta=False,
                       compatibility=dict(version='2.8.42', branch='public', sourceHashes={p['path']: p['sourceSHA256'] for p in adaptations}),
                       files={}, links={}, installedBytes=0)
        for path in sorted(package.rglob('*')):
            name = str(path.relative_to(package))
            if path.is_symlink():
                catalog['links'][name] = os.readlink(path)
            elif path.is_file():
                catalog['files'][name] = digest(path)
                catalog['installedBytes'] += path.stat().st_size
        import hashlib
        catalog['identifier'] = 'we-2.8.42-arm64-' + hashlib.sha256(json.dumps(catalog['files'], sort_keys=True).encode()).hexdigest()[:12]
        (package / 'catalog.json').write_text(json.dumps(catalog, indent=2) + '\n')
        if destination.exists():
            shutil.rmtree(destination)
        shutil.move(package, destination)
    print('Packaged native ARM64 editor support:', destination)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-root', type=Path, default=PROJECT / 'build/native-arm64/hadron')
    parser.add_argument('--output', type=Path, default=PROJECT / 'dist/EditorSupport/arm64')
    args = parser.parse_args()
    prepare(args.native_root, args.output)
