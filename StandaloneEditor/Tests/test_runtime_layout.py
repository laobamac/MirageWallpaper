#!/usr/bin/env python3
# Copyright © 2026 王孝慈. All rights reserved.

from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from runtime_layout import audit, make_portable


class RuntimeLayoutTests(unittest.TestCase):
    def test_relocated_dependency_runs_without_original_directory(self):
        with tempfile.TemporaryDirectory(prefix='mirage-layout-') as directory:
            root = Path(directory)
            external, runtime = root / 'external', root / 'runtime'
            external.mkdir(); runtime.mkdir()
            (root / 'value.c').write_text('int value(void) { return 42; }\n')
            (root / 'main.c').write_text('extern int value(void); int main(void) { return value() != 42; }\n')
            subprocess.run(['clang', '-dynamiclib', str(root / 'value.c'), '-o', str(external / 'libvalue.dylib'),
                            '-Wl,-install_name,@rpath/libvalue.dylib'], check=True)
            subprocess.run(['clang', str(root / 'main.c'), '-L' + str(external), '-lvalue',
                            '-Wl,-rpath,' + str(external), '-o', str(runtime / 'probe')], check=True)
            (external / 'libalias.dylib').symlink_to('libvalue.dylib')
            make_portable(runtime, [external / 'libalias.dylib'])
            external.rename(root / 'original-hidden')
            self.assertEqual(audit(runtime, platform.machine()), 2)
            self.assertEqual((runtime / 'lib/external/libalias.dylib').resolve(), (runtime / 'lib/external/libvalue.dylib').resolve())
            # install_name_tool invalidates ARM signatures; sign after relocation.
            for binary in [runtime / 'lib/external/libvalue.dylib', runtime / 'probe']:
                subprocess.run(['codesign', '--force', '--sign', '-', str(binary)], check=True, capture_output=True)
            subprocess.run([str(runtime / 'probe')], check=True)

    def test_arm64_payload_rejects_intel_binary(self):
        with tempfile.TemporaryDirectory(prefix='mirage-layout-') as directory:
            root = Path(directory)
            source = root / 'value.c'
            source.write_text('int value(void) { return 42; }\n')
            subprocess.run(['clang', '-arch', 'x86_64', '-dynamiclib', str(source), '-o', str(root / 'intel.dylib')], check=True)
            with self.assertRaisesRegex(ValueError, 'architecture'):
                audit(root, 'arm64')

    def test_external_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='mirage-layout-') as directory:
            root = Path(directory)
            (root / 'escaping').symlink_to('/usr/lib')
            with self.assertRaisesRegex(ValueError, 'symlink'):
                audit(root, 'arm64')


if __name__ == '__main__':
    unittest.main()
