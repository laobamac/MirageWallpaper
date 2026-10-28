#!/usr/bin/env python3
# Copyright © 2026 王孝慈. All rights reserved.

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from build_native_arm64 import cached_source_matches


class NativeBuildCacheTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='mirage-native-cache-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.git('init', '--quiet')
        self.git('config', 'user.name', 'Mirage cache test')
        self.git('config', 'user.email', 'cache-test@mirage.invalid')
        self.file = self.source / 'header.h'
        self.file.write_text('int original(void);\n')
        self.git('add', '.')
        self.git('commit', '--quiet', '-m', 'Pinned upstream')
        self.base = self.git('rev-parse', 'HEAD').decode().strip()
        self.file.write_text('int native_arm64(void);\n')
        self.git('commit', '--quiet', '-am', 'Native patch')
        self.patch = self.root / '0001-native.patch'
        self.patch.write_bytes(self.git('format-patch', '-1', '--stdout'))

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.source), *args], stderr=subprocess.PIPE)

    def matches(self):
        return cached_source_matches(self.source, self.base, [self.patch])

    def test_reapplied_patch_can_reuse_compiled_headers(self):
        self.git('checkout', '--quiet', '--detach', self.base)
        self.git('am', '--quiet', str(self.patch))
        modified = self.file.stat().st_mtime_ns
        self.assertTrue(self.matches())
        self.assertEqual(self.file.stat().st_mtime_ns, modified)

    def test_modified_source_is_not_reused(self):
        self.file.write_text('int unverified(void);\n')
        self.assertFalse(self.matches())

    def test_changed_patch_is_not_reused(self):
        self.patch.write_bytes(self.patch.read_bytes().replace(b'+int native_arm64(void);', b'+int another_patch(void);'))
        self.assertFalse(self.matches())

    def test_missing_or_extra_patch_is_not_reused(self):
        self.git('checkout', '--quiet', '--detach', self.base)
        self.assertFalse(self.matches())
        self.git('am', '--quiet', str(self.patch))
        self.file.write_text('int extra_patch(void);\n')
        self.git('commit', '--quiet', '-am', 'Unexpected extra patch')
        self.assertFalse(self.matches())


if __name__ == '__main__':
    unittest.main()
