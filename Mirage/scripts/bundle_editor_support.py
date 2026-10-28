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

app, root, architectures = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3].split()
source = Path(os.environ.get('MIRAGE_EDITOR_SUPPORT', root / 'StandaloneEditor/dist/EditorSupport'))
target = app / 'Contents/Resources/EditorSupport'
if target.exists(): shutil.rmtree(target)
if 'x86_64' not in architectures:
    sys.exit(0)
if not (source / 'catalog.json').is_file():
    if os.environ.get('CONFIGURATION') == 'Release' or os.environ.get('MIRAGE_REQUIRE_EDITOR_SUPPORT') == '1':
        sys.exit('Editor support is missing. Run StandaloneEditor/tools/package_support.py or set MIRAGE_EDITOR_SUPPORT.')
    print('Editor support is absent in this development build.')
    sys.exit(0)
catalog = json.loads((source / 'catalog.json').read_text())
for name, expected in catalog['files'].items():
    path = source / name
    if not path.resolve().is_relative_to(source.resolve()) or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        sys.exit('Editor support input failed verification: ' + name)
shutil.copytree(source, target, symlinks=True)
identity = os.environ.get('EXPANDED_CODE_SIGN_IDENTITY') or '-'
entitlements = root / 'StandaloneEditor/src/ManagedRuntime.entitlements'
sign = ['codesign', '--force', '--sign', identity, '--options', 'runtime']
if identity != '-': sign.append('--timestamp')
for path in sorted(target.rglob('*')):
    if path.is_symlink() or not path.is_file(): continue
    with path.open('rb') as stream: magic = stream.read(4)
    if magic in [b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe']:
        subprocess.run(sign + ['--entitlements', str(entitlements), str(path)], check=True, capture_output=True)
for path in sorted(target.rglob('*.app'), key=lambda p: len(p.parts), reverse=True):
    subprocess.run(sign + ['--entitlements', str(entitlements), str(path)], check=True, capture_output=True)
    subprocess.run(['codesign', '--verify', '--strict', str(path)], check=True, capture_output=True)
catalog['files'] = {}
catalog['installedBytes'] = 0
for path in sorted(target.rglob('*')):
    if path.is_file() and not path.is_symlink() and path.name != 'catalog.json':
        catalog['files'][str(path.relative_to(target))] = hashlib.sha256(path.read_bytes()).hexdigest()
        catalog['installedBytes'] += path.stat().st_size
(target / 'catalog.json').write_text(json.dumps(catalog, indent=2) + '\n')
print('Editor runtime embedded and signed; original application files will come from Steam.')
