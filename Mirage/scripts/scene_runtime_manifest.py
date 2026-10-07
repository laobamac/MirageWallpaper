__copyright__ = "Copyright © 2026 王孝慈. All rights reserved."

import argparse
import hashlib
import json
from pathlib import Path
import plistlib


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def record(app, verify=False):
    contents = app.resolve() / 'Contents'
    shared = contents / 'Extensions/MirageWallpaperExtension.appex/Contents'
    paths = list((shared / 'Frameworks').glob('*.dylib'))
    paths += list((shared / 'Resources/assets').rglob('*'))
    paths.append(shared / 'Resources/vulkan/icd.d/MoltenVK_icd.json')
    paths.append(contents / 'Resources/Renderers/vulkan/icd.d/MoltenVK_icd.json')
    for path in paths:
        if not path.resolve().is_relative_to(contents):
            raise ValueError(f'Runtime resource escapes the bundle: {path}')
    files = {str(path.relative_to(contents)): digest(path) for path in sorted(paths)
             if path.is_file() and not path.is_symlink()}
    if not all((shared / 'Frameworks' / name).is_file() for name in ('libMirageSceneSaver.dylib', 'libMoltenVK.dylib')):
        raise ValueError('Shared scene runtime is incomplete')
    manifest = dict(abi=1, files=files)
    data = (json.dumps(manifest, sort_keys=True, separators=(',', ':')) + '\n').encode()
    fingerprint = hashlib.sha256(data).hexdigest()
    target = shared / 'Resources/scene-runtime.json'
    info = plistlib.loads((contents / 'Info.plist').read_bytes())
    components = [contents] + list((contents / 'Extensions').glob('*.appex/Contents')) + list((contents / 'Resources/Screen Savers').glob('*.saver/Contents'))
    if verify:
        if target.read_bytes() != data:
            raise ValueError('Shared runtime changed after packaging')
        for component in components:
            metadata = plistlib.loads((component / 'Info.plist').read_bytes())
            if metadata.get('MirageSceneRuntimeFingerprint') != fingerprint or metadata.get('MirageSceneRuntimeABI') != 1:
                raise ValueError(f'Runtime identity mismatch: {component}')
    else:
        target.write_bytes(data)
        for component in components:
            path = component / 'Info.plist'
            metadata = plistlib.loads(path.read_bytes())
            metadata.update(MirageSceneRuntimeFingerprint=fingerprint, MirageSceneRuntimeABI=1,
                            MirageHostBundleIdentifier=info['CFBundleIdentifier'])
            path.write_bytes(plistlib.dumps(metadata))
    print(f'Shared runtime verified: {len(files)} files, ABI 1')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('app', type=Path)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    record(args.app, args.verify)
