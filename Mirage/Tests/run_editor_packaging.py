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
