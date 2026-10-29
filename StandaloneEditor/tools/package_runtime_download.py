#!/usr/bin/env python3
# Copyright © 2026 王孝慈. All rights reserved.
"""Sign an existing public support payload and emit a pinned on-demand archive."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
from urllib.parse import urlparse

PROJECT = Path(__file__).resolve().parents[1]
ROOT = PROJECT.parent


def package(support, output, base_url, identity, notary_profile=None, notary_keychain=None):
    parsed = urlparse(base_url)
    if (parsed.scheme != 'https' or parsed.netloc != 'github.com' or
            not parsed.path.startswith('/laobamac/MirageWallpaper/releases/download/') or parsed.query or parsed.fragment):
        raise ValueError('Use a versioned Mirage GitHub release URL.')
    catalog = json.loads((support / 'catalog.json').read_text())
    architecture = catalog['architecture']
    for name in list(catalog['files']) + list(catalog['links']):
        if name.startswith(('runtime/windows/', 'runtime/prefix-devel/', 'runtime/originals/', 'runtime/private/')):
            raise ValueError('Editor archives cannot contain original application files or user prefixes.')
    spec = importlib.util.spec_from_file_location('editor_bundle', ROOT / 'Mirage/scripts/bundle_editor_support.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='editor-download-', dir=output) as temporary:
        app = Path(temporary) / 'Mirage.app'
        module.bundle(app, ROOT, [architecture], {
            'MIRAGE_EDITOR_SUPPORT': str(support.resolve()), 'EXPANDED_CODE_SIGN_IDENTITY': identity,
        })
        payload = app / 'Contents/Resources/EditorSupport'
        catalog = json.loads((payload / 'catalog.json').read_text())
        if notary_profile:
            submission = Path(temporary) / 'EditorSupport.zip'
            subprocess.run(['ditto', '-c', '-k', '--keepParent', str(payload), str(submission)], check=True)
            credentials = ['--keychain-profile', notary_profile]
            if notary_keychain:
                credentials += ['--keychain', notary_keychain]
            result = subprocess.run(['xcrun', 'notarytool', 'submit', str(submission), *credentials,
                                     '--wait', '--output-format', 'json'], capture_output=True, text=True)
            response = json.loads(result.stdout)
            if result.returncode or response.get('status') != 'Accepted':
                if response.get('id'):
                    subprocess.run(['xcrun', 'notarytool', 'log', response['id'], *credentials], check=False)
                raise ValueError('Editor runtime notarization was not accepted.')
            for bundle in sorted(payload.rglob('*.app'), key=lambda p: len(p.parts), reverse=True):
                for action in ['staple', 'validate']:
                    subprocess.run(['bash', str(ROOT / 'Mirage/scripts/stapler_with_retry.sh'), action, str(bundle)], check=True)
            catalog['files'] = {}
            catalog['installedBytes'] = 0
            for path in sorted(payload.rglob('*')):
                if path.is_file() and not path.is_symlink() and path != payload / 'catalog.json':
                    catalog['files'][str(path.relative_to(payload))] = hashlib.sha256(path.read_bytes()).hexdigest()
                    catalog['installedBytes'] += path.stat().st_size
        fingerprint = hashlib.sha256(json.dumps(catalog['files'], sort_keys=True).encode()).hexdigest()
        catalog['identifier'] = 'we-' + catalog['compatibility']['version'] + '-' + architecture + '-' + fingerprint[:16]
        (payload / 'catalog.json').write_text(json.dumps(catalog, indent=2) + '\n')
        archive = Path(temporary) / 'runtime.tar.gz'
        def public_metadata(member):
            member.uid = member.gid = 0
            member.uname = member.gname = ''
            member.pax_headers = {}
            return member
        with tarfile.open(archive, 'w:gz', format=tarfile.PAX_FORMAT) as stream:
            stream.add(payload, arcname='EditorSupport', filter=public_metadata)
        with archive.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        filename = 'EditorRuntime-' + architecture + '-' + digest[:16] + '.tar.gz'
        destination = output / filename
        if destination.exists():
            raise ValueError('Refusing to replace an existing versioned archive.')
        archive.rename(destination)
        catalog['archive'] = {'url': base_url.rstrip('/') + '/' + filename,
                              'sha256': digest, 'bytes': destination.stat().st_size}
        (output / (architecture + '.json')).write_text(json.dumps(catalog, indent=2) + '\n')
        print(json.dumps({'architecture': architecture, 'archive': str(destination),
                          'bytes': destination.stat().st_size, 'sha256': digest}, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--support', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--base-url', required=True)
    parser.add_argument('--sign-identity', required=True)
    parser.add_argument('--notary-profile')
    parser.add_argument('--notary-keychain')
    args = parser.parse_args()
    package(args.support, args.output, args.base_url, args.sign_identity, args.notary_profile, args.notary_keychain)
