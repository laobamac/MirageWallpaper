#!/usr/bin/env python3
#
#  bundle_editor_support.py
#  Mirage Wallpaper
#
#  Created on 2026/10/28.
#  Copyright © 2026 王孝慈. All rights reserved.
#

"""Embed and sign the separately prepared editor support payload."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

def bundle(app, root, architectures, environment):
    source = Path(environment.get('MIRAGE_EDITOR_SUPPORT', root / 'StandaloneEditor/dist/EditorSupport'))
    target = app / 'Contents/Resources/EditorSupport'
    required = environment.get('MIRAGE_REQUIRE_EDITOR_SUPPORT') == '1' or 'MIRAGE_EDITOR_SUPPORT' in environment
    if target.exists(): shutil.rmtree(target)
    # Release apps carry only pinned download metadata. An explicit local payload
    # remains available for offline development and installation regression tests.
    manifests = Path(environment.get('MIRAGE_EDITOR_RUNTIME_MANIFESTS', root / 'StandaloneEditor/releases'))
    if 'MIRAGE_EDITOR_SUPPORT' not in environment and (manifests.exists() or 'MIRAGE_EDITOR_RUNTIME_MANIFESTS' in environment):
        for architecture in architectures:
            manifest = manifests / (architecture + '.json')
            if not manifest.is_file():
                sys.exit('error: Editor runtime download manifest is missing for ' + architecture)
            catalog = json.loads(manifest.read_text())
            archive = catalog.get('archive', {})
            from urllib.parse import urlparse
            url = urlparse(archive.get('url', ''))
            if (catalog.get('architecture') != architecture or not catalog.get('files') or
                url.scheme != 'https' or url.netloc != 'github.com' or
                not url.path.startswith('/laobamac/MirageWallpaper/releases/download/') or
                len(archive.get('sha256', '')) != 64 or any(c not in '0123456789abcdef' for c in archive['sha256']) or
                not 0 < archive.get('bytes', 0) <= 10 * 1024 ** 3):
                sys.exit('error: Invalid editor runtime download manifest: ' + str(manifest))
            destination = target if len(architectures) == 1 else target / architecture
            destination.mkdir(parents=True, exist_ok=True)
            shutil.copy2(manifest, destination / 'catalog.json')
        print('Editor runtime download manifests embedded; components are fetched only during editor installation.')
        return
    selected = []
    flat = source / 'catalog.json'
    legacy = json.loads(flat.read_text()) if flat.is_file() else None
    for architecture in architectures:
        candidate = source / architecture
        if (candidate / 'catalog.json').is_file():
            catalog = json.loads((candidate / 'catalog.json').read_text())
            if catalog['architecture'] != architecture:
                sys.exit('error: Editor support architecture does not match its directory.')
        elif legacy and legacy['architecture'] == architecture:
            candidate, catalog = source, legacy
        else:
            continue
        destination = target if len(architectures) == 1 else target / architecture
        selected.append((candidate, destination, catalog))
    if required and {item[2]['architecture'] for item in selected} != set(architectures):
        sys.exit('error: Editor support is missing for a requested architecture. Set MIRAGE_EDITOR_SUPPORT to a prepared payload.')
    if not selected:
        print('Editor support is not configured; this build has no editor runtime.')
        return
    sys.path.insert(0, str(root / 'StandaloneEditor/tools'))
    from runtime_layout import audit, is_macho
    for source, destination, catalog in selected:
        if catalog['architecture'] == 'arm64':
            if catalog.get('rosetta') is not False or catalog.get('minimumMacOS') != '26.5':
                sys.exit('error: ARM64 editor support must declare its native Wine/FEX runtime requirements.')
            audit(source, 'arm64')
        for name, expected in catalog['files'].items():
            path = source / name
            if not path.resolve().is_relative_to(source.resolve()) or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
                sys.exit('error: Editor support input failed verification: ' + name)
        for name, expected in catalog['links'].items():
            path = source / name
            if not path.is_symlink() or os.readlink(path) != expected or not path.resolve().is_relative_to(source.resolve()):
                sys.exit('error: Editor support symlink failed verification: ' + name)
        shutil.copytree(source, destination, symlinks=True)
        identity = environment.get('EXPANDED_CODE_SIGN_IDENTITY') or '-'
        entitlements = root / 'StandaloneEditor/src' / ('NativeARM64.entitlements' if catalog['architecture'] == 'arm64' else 'ManagedRuntime.entitlements')
        sign = ['codesign', '--force', '--sign', identity, '--options', 'runtime']
        if identity != '-': sign.append('--timestamp')
        for path in sorted(destination.rglob('*')):
            if is_macho(path):
                subprocess.run(sign + ['--entitlements', str(entitlements), str(path)], check=True, capture_output=True)
        for path in sorted(destination.rglob('*.app'), key=lambda p: len(p.parts), reverse=True):
            subprocess.run(sign + ['--entitlements', str(entitlements), str(path)], check=True, capture_output=True)
            subprocess.run(['codesign', '--verify', '--strict', str(path)], check=True, capture_output=True)
        catalog['files'] = {}
        catalog['installedBytes'] = 0
        for path in sorted(destination.rglob('*')):
            if path.is_file() and not path.is_symlink() and path != destination / 'catalog.json':
                catalog['files'][str(path.relative_to(destination))] = hashlib.sha256(path.read_bytes()).hexdigest()
                catalog['installedBytes'] += path.stat().st_size
        (destination / 'catalog.json').write_text(json.dumps(catalog, indent=2) + '\n')
        print('Editor runtime embedded and signed (' + catalog['architecture'] + '); original application files will come from Steam.')


if __name__ == '__main__':
    bundle(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3].split(), os.environ)
