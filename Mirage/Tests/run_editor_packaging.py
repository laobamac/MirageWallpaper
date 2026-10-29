#!/usr/bin/env python3
# Copyright © 2026 王孝慈. All rights reserved.
"""Exercise editor payload selection using a clean checkout layout."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('editor_packaging', ROOT / 'Mirage/scripts/bundle_editor_support.py')
packaging = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packaging)


class EditorPackagingTests(unittest.TestCase):
    def test_download_build_contains_only_the_pinned_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifests = root / 'StandaloneEditor/releases'
            manifests.mkdir(parents=True)
            catalog = {'architecture': 'x86_64', 'files': {'runtime/value': 'a' * 64},
                       'archive': {'url': 'https://github.com/laobamac/MirageWallpaper/releases/download/editor-runtime-test/runtime.tar.gz',
                                   'sha256': 'b' * 64, 'bytes': 1234}}
            (manifests / 'x86_64.json').write_text(json.dumps(catalog))
            packaging.bundle(root / 'Mirage.app', root, ['x86_64'], {'MIRAGE_REQUIRE_EDITOR_SUPPORT': '1'})
            payload = root / 'Mirage.app/Contents/Resources/EditorSupport'
            self.assertEqual(json.loads((payload / 'catalog.json').read_text()), catalog)
            self.assertEqual([path.name for path in payload.iterdir()], ['catalog.json'])
            self.assertLess((payload / 'catalog.json').stat().st_size, 1024)

    def test_missing_or_untrusted_download_manifest_rejects_release(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifests = root / 'StandaloneEditor/releases'
            manifests.mkdir(parents=True)
            with self.assertRaisesRegex(SystemExit, 'missing'):
                packaging.bundle(root / 'Mirage.app', root, ['arm64'], {})
            (manifests / 'arm64.json').write_text(json.dumps({'architecture': 'arm64', 'files': {'runtime/value': 'a' * 64},
                'archive': {'url': 'https://example.com/runtime.tar.gz', 'sha256': 'b' * 64, 'bytes': 1234}}))
            with self.assertRaisesRegex(SystemExit, 'Invalid'):
                packaging.bundle(root / 'Mirage.app', root, ['arm64'], {})

    def test_clean_release_without_optional_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for architecture in ['x86_64', 'arm64']:
                packaging.bundle(root / 'Mirage.app', root, [architecture], {'CONFIGURATION': 'Release'})
            self.assertFalse((root / 'Mirage.app/Contents/Resources/EditorSupport').exists())

    def test_required_or_explicit_payload_cannot_silently_disappear(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for environment in [{'MIRAGE_REQUIRE_EDITOR_SUPPORT': '1'}, {'MIRAGE_EDITOR_SUPPORT': str(root / 'missing')}]:
                with self.assertRaisesRegex(SystemExit, 'missing'):
                    packaging.bundle(root / 'Mirage.app', root, ['x86_64'], environment)

    def test_wrong_architecture_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            support = root / 'support'
            support.mkdir()
            (support / 'catalog.json').write_text(json.dumps({'architecture': 'x86_64'}))
            with self.assertRaisesRegex(SystemExit, 'architecture'):
                packaging.bundle(root / 'Mirage.app', root, ['arm64'], {'MIRAGE_EDITOR_SUPPORT': str(support)})

    def test_universal_app_requires_both_payloads_when_enabled(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            support = root / 'support/arm64'
            support.mkdir(parents=True)
            (support / 'catalog.json').write_text(json.dumps({'architecture': 'arm64'}))
            with self.assertRaisesRegex(SystemExit, 'missing'):
                packaging.bundle(root / 'Mirage.app', root, ['x86_64', 'arm64'], {'MIRAGE_EDITOR_SUPPORT': str(support.parent)})


if __name__ == '__main__':
    unittest.main()
