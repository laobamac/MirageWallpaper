#!/usr/bin/env python3
# Copyright © 2026 王孝慈. All rights reserved.
"""Rebuild the integration in an existing isolated editor runtime, without launching it."""
import argparse
import hashlib
import json
import os
import plistlib
from pathlib import Path
import shutil
import subprocess
import tempfile
from patch_native_preview import patch
from build_icon import build_icon

PROJECT = Path(__file__).resolve().parents[1]
SOURCE = PROJECT / 'src'
DXVK_SHA = '0ff0b0835dde29556bd01dfce7b1ae348d7f229cb1e1a37cc71ea1a028beeca4'

def existing(candidates):
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return str(Path(candidate).resolve())
    raise RuntimeError('Required compiler or headers not found: ' + str(candidates))

def install(source, target):
    # Replace the inode atomically; an already-running process keeps its image.
    temporary = target.with_name(target.name + '.mirage-new')
    shutil.copy2(source, temporary)
    os.replace(temporary, target)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, default=PROJECT / 'runtime')
    parser.add_argument('--original-ui', type=Path)
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    system = runtime / 'prefix-devel/drive_c/windows/system32'
    original_dxvk = system / 'mirage_dxvk.dll'
    if hashlib.sha256(original_dxvk.read_bytes()).hexdigest() != DXVK_SHA:
        raise RuntimeError('Unsupported DXVK build; original runtime must be preserved.')
    executable, native_manifest = patch((runtime / 'originals/wallpaperui.exe').read_bytes())
    ui = (args.original_ui or runtime / 'originals/scripts.js').read_text()
    manifest = json.loads((runtime / 'private/ui-patches.json').read_text())
    if hashlib.sha256(ui.encode()).hexdigest() != manifest['original_sha256']:
        raise RuntimeError('Unsupported original editor UI.')
    for change in reversed(manifest['edits']):
        if ui[change['start']:change['end']] != change['before']:
            raise RuntimeError('UI patch input does not match.')
        ui = ui[:change['start']] + change['after'] + ui[change['end']:]
    if hashlib.sha256(ui.encode()).hexdigest() != manifest['result_sha256']:
        raise RuntimeError('UI patch output does not match.')
    cellar = [Path('/opt/homebrew/Cellar'), Path('/usr/local/Cellar')]
    mingw = existing([os.environ.get('MIRAGE_MINGW_ROOT')] + [p for c in cellar for p in c.glob('mingw-w64/*/toolchain-x86_64')])
    winroot = Path(mingw) / 'x86_64-w64-mingw32'
    gcc = existing([str(Path(mingw) / 'bin/x86_64-w64-mingw32-gcc')])
    llvm = existing([os.environ.get('MIRAGE_CLANG'), '/opt/homebrew/opt/llvm/bin/clang',
                     '/usr/local/opt/llvm/bin/clang', shutil.which('clang')])
    headers = existing([os.environ.get('MIRAGE_VULKAN_HEADERS')] + [p for c in cellar for p in c.glob('vulkan-headers/*/include')])
    with tempfile.TemporaryDirectory(prefix='mirage-build-') as directory:
        output = Path(directory)
        build_icon(output / 'MirageEditor.icns')
        def run(*command):
            subprocess.run([str(x) for x in command], check=True)
        run(llvm, '--target=x86_64-w64-windows-gnu', '--sysroot=' + str(winroot), '-I' + headers,
            '-O2', '-c', SOURCE / 'D3D11MetalInterop.c', '-o', output / 'interop.o')
        run(gcc, '-shared', '-static-libgcc', output / 'interop.o', SOURCE / 'd3d11.def',
            '-o', output / 'd3d11.dll', '-ldxguid')
        run('clang', '-O2', '-dynamiclib', '-fobjc-arc', '-I' + headers,
            '-framework', 'AppKit', '-framework', 'QuartzCore', '-framework', 'Metal',
            SOURCE / 'NativeCompositor.m', SOURCE / 'WineWindowLocks.c', SOURCE / 'NativeIdentity.m',
            SOURCE / 'MetalInterop.m', SOURCE / 'ShaderOptimizer.m', '-o', output / 'MetalInterop.dylib')
        run('swiftc', SOURCE / 'Launcher.swift', '-o', output / 'Launcher')
        run('swiftc', SOURCE / 'NativeBridge.swift', '-o', output / 'ProjectPicker')
        run('node', '--check', SOURCE / 'standalone.js')
        (output / 'scripts.js').write_text(ui)
        # Avoid printing a whole minified bundle if a patch is invalid.
        run('node', '-e', "try { new (require('vm').Script)(require('fs').readFileSync(process.argv[1], 'utf8')); } "
            "catch (error) { console.error(error.message); process.exit(1); }", output / 'scripts.js')
        (output / 'wallpaperui.exe').write_bytes(executable)
        targets = {
            'd3d11.dll': system / 'd3d11.dll',
            'MetalInterop.dylib': runtime / 'MetalInterop.dylib',
            'Launcher': PROJECT / 'Mirage Editor.app/Contents/MacOS/Launcher',
            'ProjectPicker': runtime / 'Mirage Project Picker.app/Contents/MacOS/ProjectPicker',
            'scripts.js': runtime / 'windows/WallpaperEngine/ui/dist/scripts/scripts.js',
            'wallpaperui.exe': runtime / 'windows/WallpaperEngine/bin/wallpaperui.exe',
            'MirageEditor.icns': runtime / 'MirageEditor.icns',
        }
        for name, target in targets.items():
            install(output / name, target)
        for bundle in [PROJECT / 'Mirage Editor.app', runtime / 'Mirage WE Editor.app',
                       runtime / 'Mirage Project Picker.app']:
            install(output / 'MirageEditor.icns', bundle / 'Contents/Resources/MirageEditor.icns')
            info_path = bundle / 'Contents/Info.plist'
            info = plistlib.loads(info_path.read_bytes())
            info['CFBundleIconFile'] = 'MirageEditor.icns'
            temporary = info_path.with_name('Info.plist.mirage-new')
            temporary.write_bytes(plistlib.dumps(info, sort_keys=False))
            os.replace(temporary, info_path)
            os.utime(bundle, None)
        for name, target in {
            'standalone.js': runtime / 'windows/WallpaperEngine/ui/dist/scripts/mirage-standalone.js',
            'macos.css': runtime / 'windows/WallpaperEngine/ui/dist/styles/macos.css',
            'dxvk.conf': runtime / 'dxvk.conf',
        }.items():
            install(SOURCE / name, target)
    (runtime / 'native-preview-patch.json').write_text(json.dumps(native_manifest, ensure_ascii=False, indent=2))
    print('Rebuilt standalone editor integration and particle compatibility patch. No editor process was restarted.')

if __name__ == '__main__':
    main()
