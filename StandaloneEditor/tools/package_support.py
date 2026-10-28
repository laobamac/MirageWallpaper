#!/usr/bin/env python3
#
#  package_support.py
#  Mirage Wallpaper
#
#  Created on 2026/10/28.
#  Copyright © 2026 王孝慈. All rights reserved.
#

"""Package prebuilt adapters and Wine; never include WE files or a user's prefix."""

import argparse
import base64
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from patch_native_preview import patch

PROJECT = Path(__file__).resolve().parents[1]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def copy_operation(offset, count):
    return {"offset": offset, "count": count}


def literal(data):
    return {"data": base64.b64encode(data).decode()}


def ui_patch(runtime, original_ui=None):
    reference = (runtime / 'originals/scripts.js').read_text()
    manifest = json.loads((runtime / 'private/ui-patches.json').read_text())
    assert digest(reference.encode()) == manifest['original_sha256']
    source = original_ui.read_text() if original_ui else reference
    edits = []
    for original_edit in manifest['edits']:
        edit = dict(original_edit)
        if source != reference:
            if edit['before']:
                assert source.count(edit['before']) == 1, 'UI adaptation anchor changed'
                edit['start'] = source.index(edit['before'])
                edit['end'] = edit['start'] + len(edit['before'])
            else:
                start = edit['start']
                anchor = reference[start - 100:start + 100]
                assert source.count(anchor) == 1, 'UI insertion anchor changed'
                edit['start'] = edit['end'] = source.index(anchor) + 100
        edits.append(edit)
    operations, cursor, output = [], 0, []
    for edit in sorted(edits, key=lambda item: item['start']):
        start, end = edit['start'], edit['end']
        assert source[start:end] == edit['before'] and start >= cursor
        operations.append(copy_operation(len(source[:cursor].encode()), len(source[cursor:start].encode())))
        output.append(source[cursor:start])
        before, after = edit['before'].encode(), edit['after'].encode()
        origin = len(source[:start].encode())
        for kind, a, b, c, d in difflib.SequenceMatcher(None, before, after, autojunk=False).get_opcodes():
            if kind == 'equal': operations.append(copy_operation(origin + a, b - a))
            elif kind in ('insert', 'replace'): operations.append(literal(after[c:d]))
        output.append(edit['after'])
        cursor = end
    operations.append(copy_operation(len(source[:cursor].encode()), len(source[cursor:].encode())))
    output.append(source[cursor:])
    result = ''.join(output).encode()
    subprocess.run(['node', '-e', "new (require('vm').Script)(require('fs').readFileSync(0, 'utf8'))"],
                   input=result, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    return dict(path='ui/dist/scripts/scripts.js', sourceSHA256=digest(source.encode()),
                resultSHA256=digest(result), operations=operations)


def executable_patch(runtime):
    source = (runtime / 'originals/wallpaperui.exe').read_bytes()
    result, _ = patch(source)
    operations, cursor = [], 0
    while cursor < len(source):
        same = source[cursor] == result[cursor]
        end = cursor + 1
        while end < len(source) and (source[end] == result[end]) == same:
            end += 1
        operations.append(copy_operation(cursor, end - cursor) if same else literal(result[cursor:end]))
        cursor = end
    if len(result) > len(source): operations.append(literal(result[len(source):]))
    return dict(path='bin/wallpaperui.exe', sourceSHA256=digest(source), resultSHA256=digest(result), operations=operations)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, default=PROJECT / 'runtime')
    parser.add_argument('--output', type=Path, default=PROJECT / 'dist/EditorSupport')
    parser.add_argument('--original-ui', type=Path, help='Migrate validated anchors to a new official UI file.')
    parser.add_argument('--update-adaptations', action='store_true')
    args = parser.parse_args()
    runtime, destination = args.runtime.resolve(), args.output.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='editor-package-', dir=destination.parent) as temporary:
        output = Path(temporary)
        target = output / 'runtime'
        target.mkdir()
        for name in ['Mirage WE Editor.app', 'Mirage Project Picker.app']:
            shutil.copytree(runtime / name, target / name, symlinks=True,
                            ignore=shutil.ignore_patterns('.DS_Store', '_CodeSignature'))
        shutil.copytree(PROJECT / 'Mirage Editor.app', output / 'Mirage Editor.app', symlinks=True,
                        ignore=shutil.ignore_patterns('.DS_Store', '_CodeSignature'))
        # Keep ntdll and its data together inside the identity app. Wine locates
        # its library and locale directories relative to ntdll's resolved path.
        identity = target / 'Mirage WE Editor.app/Contents'
        shutil.copytree(runtime / 'engines/Wine Devel.app/Contents/Resources/wine',
                        identity / 'Resources/wine', symlinks=True,
                        ignore=shutil.ignore_patterns('.DS_Store', '_CodeSignature'))
        ntdll = identity / 'MacOS/ntdll.so'
        ntdll.unlink()
        ntdll.symlink_to('../Resources/wine/lib/wine/x86_64-unix/ntdll.so')
        subprocess.run(['swiftc', '-D', 'MIRAGE_MANAGED_EDITOR', str(PROJECT / 'src/Launcher.swift'),
                        '-o', str(output / 'Mirage Editor.app/Contents/MacOS/Launcher')], check=True)
        subprocess.run(['swiftc', '-D', 'MIRAGE_MANAGED_EDITOR', str(PROJECT / 'src/NativeBridge.swift'),
                        '-o', str(target / 'Mirage Project Picker.app/Contents/MacOS/ProjectPicker')], check=True)
        for name in ['MetalInterop.dylib', 'MirageEditor.icns', 'standalone-defaults.json']:
            shutil.copy2(runtime / name, target / name)
        for name in ['standalone.js', 'macos.css', 'dxvk.conf']:
            shutil.copy2(PROJECT / 'src' / name, target / name)
        (target / 'overrides').mkdir()
        for name in ['d3d11.dll', 'd3d10core.dll', 'mirage_dxvk.dll']:
            shutil.copy2(runtime / 'prefix-devel/drive_c/windows/system32' / name, target / 'overrides' / name)
        recipes = PROJECT / 'src/EditorAdaptations.json'
        if args.update_adaptations or not recipes.exists():
            adaptations = [executable_patch(runtime), ui_patch(runtime, args.original_ui)]
            if args.update_adaptations:
                recipes.write_text(json.dumps({'created': '2026/10/28', 'copyright': 'Copyright © 2026 王孝慈. All rights reserved.',
                                               'patches': adaptations}, indent=2) + '\n')
        else:
            adaptations = json.loads(recipes.read_text())['patches']
        (target / 'adaptations.json').write_text(json.dumps(adaptations, separators=(',', ':')))
        shutil.copytree(PROJECT / 'Licenses', output / 'Licenses')
        shutil.copy2(PROJECT.parent / 'SteamService/Licenses/LGPL-2.1.txt', output / 'Licenses/Wine-LGPL-2.1.txt')
        (output / 'LICENSE-NOTICE.txt').write_text(
            'Mirage Editor adapters: Copyright © 2026 王孝慈. All rights reserved.\n'
            'No Wallpaper Engine binaries, resources, original UI, or initialized Wine prefix are included.\n'
            'Wine: LGPL-2.1-or-later, https://gitlab.winehq.org/wine/wine (wine-11.18).\n'
            'DXVK: zlib, https://github.com/doitsujin/dxvk/tree/v1.10.3\n'
            'MoltenVK: Apache-2.0, https://github.com/KhronosGroup/MoltenVK\n')
        catalog = dict(schema=1, identifier='we-2.8.42-adapter-1', architecture='x86_64', installedBytes=0,
                       compatibility=dict(version='2.8.42', branch='public', sourceHashes={p['path']:p['sourceSHA256'] for p in adaptations}),
                       files={}, links={})
        for path in sorted(output.rglob('*')):
            relative = str(path.relative_to(output))
            if path.is_symlink():
                link = os.readlink(path)
                if os.path.isabs(link) or not path.resolve().is_relative_to(output):
                    raise ValueError('External runtime symlink: ' + relative)
                catalog['links'][relative] = link
            elif path.is_file():
                catalog['files'][relative] = digest(path.read_bytes())
                catalog['installedBytes'] += path.stat().st_size
        catalog['identifier'] = 'we-2.8.42-' + digest(json.dumps(catalog['files'], sort_keys=True).encode())[:12]
        (output / 'catalog.json').write_text(json.dumps(catalog, indent=2) + '\n')
        if destination.exists(): shutil.rmtree(destination)
        shutil.move(output, destination)
    print(f'Packaged editor support: {destination} (no Wallpaper Engine content)')


if __name__ == '__main__':
    main()
