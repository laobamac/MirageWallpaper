#!/bin/bash
set -euo pipefail
COPYRIGHT="Copyright © 2026 王孝慈. All rights reserved."
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
python3 - "${1:?Usage: trim_assets.sh <staged-app-assets>}" "$ROOT/assets" <<'PY'
import json
import os
from pathlib import Path
import shutil
import sys

root = Path(sys.argv[1]).absolute()
source = Path(sys.argv[2]).resolve()
if root.resolve() != root or root == source or source in root.parents:
    raise SystemExit('Refusing to trim source assets or a symlink path')
if root.parts[-3:] != ('Contents', 'Resources', 'assets') or root.parents[2].suffix != '.app':
    raise SystemExit('Only staged .app/Contents/Resources/assets may be trimmed')
if not root.is_dir() or not (root.parents[1] / 'Info.plist').is_file():
    raise SystemExit('Staged app is incomplete')
paths = list(root.rglob('*'))
if any(path.is_symlink() for path in paths):
    raise SystemExit('Symlinks are not allowed in staged assets')


def strings(value, key=''):
    if key == 'preview':
        return
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for k, v in value.items():
            yield from strings(v, k)
    elif isinstance(value, list):
        for v in value:
            yield from strings(v)


def previews(value):
    if isinstance(value, dict):
        for k, v in value.items():
            if k == 'preview' and isinstance(v, str):
                yield v
            else:
                yield from previews(v)
    elif isinstance(value, list):
        for v in value:
            yield from previews(v)


candidates = set()
documents = []
for path in paths:
    if path.suffix != '.json':
        continue
    try:
        value = json.loads(path.read_text())
    except (UnicodeError, ValueError):
        continue
    documents.append((path, value))
    relative = path.relative_to(root)
    if len(relative.parts) == 3 and relative.parts[0] in ('effects', 'presets'):
        for preview in previews(value):
            directory = (path.parent / preview).parent
            if directory.parent == path.parent and directory.name.startswith('preview') and directory.is_dir():
                candidates.add(directory)

protected = set()
for path, value in documents:
    if candidates.intersection(path.parents):
        continue
    for text in strings(value):
        if len(text) > 2048 or '\x00' in text or '\n' in text:
            continue
        text = text.replace('\\', '/')
        if text.startswith('/assets/'):
            text = text[8:]
        for base in (root, path.parent):
            reference = Path(os.path.normpath(base / text))
            protected.update(candidates.intersection((reference, *reference.parents)))

removed = 0
for directory in sorted(candidates - protected):
    removed += sum(p.stat().st_size for p in directory.rglob('*') if p.is_file())
    shutil.rmtree(directory)
for path in list(root.rglob('*')):
    if path.is_file() and (path.name == '.DS_Store' or (path.suffix.lower() == '.tga' and path.with_suffix('.tex').is_file())):
        removed += path.stat().st_size
        path.unlink()
print(f'Editor previews/source duplicates removed: {removed / 1048576:.2f} MiB; runtime materials retained')
PY
