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
