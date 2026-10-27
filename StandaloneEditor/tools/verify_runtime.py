#!/usr/bin/env python3
# Copyright © 2026 王孝慈. All rights reserved.
"""Validate the organized local editor without launching or changing it."""

import hashlib
import json
import plistlib
from pathlib import Path

from patch_native_preview import patch
from build_runtime import AUDIO_USAGE

PROJECT = Path(__file__).resolve().parents[1]
RUNTIME = PROJECT / 'runtime'
SOURCE = PROJECT / 'src'


def verify():
    required = [
        PROJECT / 'Mirage Editor.app/Contents/MacOS/Launcher',
        RUNTIME / 'Mirage Project Picker.app/Contents/MacOS/ProjectPicker',
        RUNTIME / 'Mirage WE Editor.app/Contents/MacOS/wine',
        RUNTIME / 'Mirage WE Editor.app/Contents/MacOS/ntdll.so',
        RUNTIME / 'engines/Wine Devel.app/Contents/Resources/wine/bin/wine',
        RUNTIME / 'MetalInterop.dylib',
    ]
    for path in required:
        if not path.is_file():
            raise ValueError('Missing runtime file: ' + str(path))
    icon = (RUNTIME / 'MirageEditor.icns').read_bytes()
    if not icon.startswith(b'icns'):
        raise ValueError('Invalid editor icon package')
    for bundle in [PROJECT / 'Mirage Editor.app', RUNTIME / 'Mirage WE Editor.app',
                   RUNTIME / 'Mirage Project Picker.app']:
        if (bundle / 'Contents/Resources/MirageEditor.icns').read_bytes() != icon:
            raise ValueError('Editor icon differs: ' + str(bundle))
        info = plistlib.loads((bundle / 'Contents/Info.plist').read_bytes())
        if info.get('CFBundleIconFile') != 'MirageEditor.icns':
            raise ValueError('Editor icon is not selected: ' + str(bundle))
        if info.get('NSAudioCaptureUsageDescription') != AUDIO_USAGE['en']:
            raise ValueError('Missing system audio permission description: ' + str(bundle))
        for language, usage in AUDIO_USAGE.items():
            localized = bundle / 'Contents/Resources' / (language + '.lproj/InfoPlist.strings')
            if not localized.is_file() or json.dumps(usage, ensure_ascii=False) not in localized.read_text():
                raise ValueError('Missing localized audio permission: ' + str(localized))

    expected, manifest = patch((RUNTIME / 'originals/wallpaperui.exe').read_bytes())
    if (RUNTIME / 'windows/WallpaperEngine/bin/wallpaperui.exe').read_bytes() != expected:
        raise ValueError('Editor executable differs from the source patch')
    ui_manifest = json.loads((RUNTIME / 'private/ui-patches.json').read_text())
    ui = (RUNTIME / 'originals/scripts.js').read_text()
    if hashlib.sha256(ui.encode()).hexdigest() != ui_manifest['original_sha256']:
        raise ValueError('Original UI version does not match')
    for edit in reversed(ui_manifest['edits']):
        if ui[edit['start']:edit['end']] != edit['before']:
            raise ValueError('UI patch input differs')
        ui = ui[:edit['start']] + edit['after'] + ui[edit['end']:]
    if hashlib.sha256(ui.encode()).hexdigest() != ui_manifest['result_sha256']:
        raise ValueError('UI patch result hash differs')
    if (RUNTIME / 'windows/WallpaperEngine/ui/dist/scripts/scripts.js').read_text() != ui:
        raise ValueError('Installed UI differs from source')
    for source, target in [
        ('standalone.js', 'windows/WallpaperEngine/ui/dist/scripts/mirage-standalone.js'),
        ('macos.css', 'windows/WallpaperEngine/ui/dist/styles/macos.css'),
        ('dxvk.conf', 'dxvk.conf'),
    ]:
        if (SOURCE / source).read_bytes() != (RUNTIME / target).read_bytes():
            raise ValueError('Installed file differs: ' + source)
    config = [line.strip() for line in (RUNTIME / 'dxvk.conf').read_text().splitlines()
              if line.strip() and not line.lstrip().startswith('#')]
    if config != ['dxvk.enableDebugUtils = True']:
        raise ValueError('Unexpected graphics configuration override')
    drive = RUNTIME / 'prefix-devel/dosdevices/m:'
    if not drive.is_symlink() or drive.readlink().is_absolute() or drive.resolve() != RUNTIME / 'windows':
        raise ValueError('Editor drive mapping is not relative to the runtime')
    installed = (RUNTIME / 'windows/WallpaperEngine/ui/dist/index.html').read_text()
    if 'mirage-diagnostics.js' in installed or 'miragePreview(' in ui:
        raise ValueError('Experimental preview or diagnostic code remains')
    preview_patch = next((edit['after'] for edit in ui_manifest['edits']
                          if 'window.mirageEditor.createPreview' in edit['after']), '')
    if not preview_patch or 'createShader' in preview_patch:
        raise ValueError('Preview resource management patch is missing')
    build = json.loads((RUNTIME / 'integration-build.json').read_text())
    for name, digest in build['sources'].items():
        if hashlib.sha256((PROJECT / name).read_bytes()).hexdigest() != digest:
            raise ValueError('Integration needs rebuilding: ' + name)
    for name, digest in build['binaries'].items():
        if hashlib.sha256((RUNTIME / name).read_bytes()).hexdigest() != digest:
            raise ValueError('Integration binary differs: ' + name)
    if hashlib.sha256((RUNTIME / 'private/ui-patches.json').read_bytes()).hexdigest() != build['ui_manifest_sha256']:
        raise ValueError('UI integration needs rebuilding')
    print('Passed: native build/source hashes, localized audio permission, preview lifecycle, version-locked patches and drive mapping.')
    print('Graphics configuration contains only the required texture-sharing flag.')
    print('Editor SHA-256: ' + manifest['sha256'])


if __name__ == '__main__':
    verify()
