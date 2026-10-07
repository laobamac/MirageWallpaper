__copyright__ = "Copyright © 2026 王孝慈. All rights reserved."

import hashlib
import os
import re
import argparse
import uuid
import importlib.util
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import unittest
import sys
from unittest.mock import Mock, patch
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
TRIM = ROOT / 'Mirage/scripts/trim_assets.sh'
APP_PATH = None
SPEC = importlib.util.spec_from_file_location('runtime_manifest', ROOT / 'Mirage/scripts/scene_runtime_manifest.py')
RUNTIME = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNTIME)
CODEC_SPEC = importlib.util.spec_from_file_location('codec_packaging', ROOT / 'Mirage/Tests/run_codec_packaging.py')
CODECS = importlib.util.module_from_spec(CODEC_SPEC)
CODEC_SPEC.loader.exec_module(CODECS)


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='mirage-packaging-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.app = self.root / 'Fixture.app'
        self.assets = self.app / 'Contents/Resources/assets'
        self.assets.mkdir(parents=True)
        (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(CFBundleIdentifier='cn.laobamac.Mirage')))

    def trim(self, path=None):
        return subprocess.run(['bash', str(TRIM), str(path or self.assets)], capture_output=True, text=True, timeout=30)

    def put(self, path, data=b'payload'):
        target = self.assets / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return target

    def test_explicit_editor_previews_only(self):
        self.put('effects/test/effect.json', json.dumps(dict(preview='preview/project.json')).encode())
        self.put('effects/test/preview/project.json', b'{}')
        self.put('effects/test/preview/materials/texture.tex')
        self.put('materials/preview_runtime/texture.tex')
        self.put('materials/editor/needed.tex')
        self.put('materials/duplicate.tga')
        self.put('materials/duplicate.tex')
        self.put('materials/unique.tga')
        self.put('.DS_Store')
        before = self.put('materials/runtime.tex').read_bytes()
        result = self.trim()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.assets / 'effects/test/preview').exists())
        self.assertFalse((self.assets / 'materials/duplicate.tga').exists())
        for path in ['materials/editor/needed.tex', 'materials/preview_runtime/texture.tex', 'materials/unique.tga', 'materials/duplicate.tex']:
            self.assertTrue((self.assets / path).exists(), path)
        self.assertEqual((self.assets / 'materials/runtime.tex').read_bytes(), before)
        self.assertEqual(self.trim().returncode, 0)

    def test_non_preview_reference_preserves_candidate(self):
        self.put('effects/test/effect.json', b'{"preview":"preview/project.json","material":"preview/runtime.json"}')
        self.put('effects/test/preview/project.json', b'{}')
        self.put('effects/test/preview/runtime.json', b'{}')
        self.assertEqual(self.trim().returncode, 0)
        self.assertTrue((self.assets / 'effects/test/preview/runtime.json').is_file())

    def test_reject_source_or_arbitrary_target(self):
        self.assertNotEqual(self.trim(ROOT / 'assets').returncode, 0)
        self.assertNotEqual(self.trim(self.root).returncode, 0)

    def test_reject_symlink_payload_without_writing(self):
        target = self.root / 'protected.tga'
        target.write_bytes(b'protected')
        (self.assets / 'escape.tga').symlink_to(target)
        self.assertNotEqual(self.trim().returncode, 0)
        self.assertEqual(target.read_bytes(), b'protected')

    def test_shared_runtime_fingerprint(self):
        shared = self.app / 'Contents/Extensions/MirageWallpaperExtension.appex/Contents'
        frameworks = shared / 'Frameworks'
        frameworks.mkdir(parents=True)
        (shared / 'Resources/assets').mkdir(parents=True)
        (shared / 'Info.plist').write_bytes(plistlib.dumps(dict(CFBundleIdentifier='cn.laobamac.Mirage.Extension')))
        shared_icd = shared / 'Resources/vulkan/icd.d/MoltenVK_icd.json'
        shared_icd.parent.mkdir(parents=True)
        shared_icd.write_text('{}')
        texture = shared / 'Resources/assets/test.tex'
        texture.write_bytes(b'old')
        for name in ['libMirageSceneSaver.dylib', 'libMoltenVK.dylib']:
            (frameworks / name).write_bytes(name.encode())
        self.put('materials/texture.tex')
        icd = self.app / 'Contents/Resources/Renderers/vulkan/icd.d/MoltenVK_icd.json'
        icd.parent.mkdir(parents=True)
        icd.write_text('{}')
        RUNTIME.record(self.app)
        RUNTIME.record(self.app, True)
        texture.write_bytes(b'updated')
        with self.assertRaises(ValueError): RUNTIME.record(self.app, True)
        RUNTIME.record(self.app)
        (frameworks / 'libMirageSceneSaver.dylib').unlink()
        with self.assertRaises(ValueError): RUNTIME.record(self.app, True)

    def test_sandboxed_shared_runtime(self):
        if APP_PATH is None:
            self.skipTest('Pass --app to test the signed App Sandbox runtime')
        host = self.root / 'SandboxHost.app'
        extension = host / 'Contents/Extensions/RuntimeProbe.appex'
        executable = extension / 'Contents/MacOS/RuntimeProbe'
        executable.parent.mkdir(parents=True)
        source = APP_PATH.resolve() / 'Contents/Extensions/MirageWallpaperExtension.appex/Contents'
        for name in ['Frameworks', 'Resources']:
            shutil.copytree(source / name, extension / 'Contents' / name, symlinks=True)
        identifier = 'cn.laobamac.Mirage.RuntimeProbe.' + uuid.uuid4().hex
        (extension / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(CFBundleIdentifier=identifier, CFBundleExecutable='RuntimeProbe', CFBundlePackageType='XPC!', CFBundleVersion='1')))
        (host / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(CFBundleIdentifier=identifier + '.Host', CFBundlePackageType='APPL', CFBundleVersion='1')))
        entitlements = self.root / 'entitlements.plist'
        entitlements.write_bytes(plistlib.dumps({'com.apple.security.app-sandbox': True}))
        subprocess.run(['xcrun', 'swiftc', ROOT / 'Mirage/Tests/SharedRuntimeSandboxProbe.swift', '-parse-as-library', '-o', executable], check=True, timeout=90)
        subprocess.run(['codesign', '--force', '--sign', '-', '--entitlements', entitlements, extension], check=True, timeout=30)
        subprocess.run([executable], check=True, timeout=30)

    def test_relocated_library_loads_without_host_runpaths(self):
        original = self.root / 'original'
        frameworks = self.root / 'relocated package/Frameworks'
        original.mkdir()
        frameworks.mkdir(parents=True)
        dependency = original / 'libfixture_dependency.1.0.dylib'
        (original / 'dependency.c').write_text('int fixture_value(void) { return 42; }\n')
        (original / 'consumer.c').write_text('extern int fixture_value(void); int fixture_read(void) { return fixture_value(); }\n')
        subprocess.run(['xcrun', 'clang', '-dynamiclib', original / 'dependency.c', '-o', dependency,
                        '-Wl,-headerpad_max_install_names', '-install_name', str(original / 'libfixture_dependency.1.dylib')], check=True, timeout=60)
        (original / 'libfixture_dependency.1.dylib').symlink_to(dependency.name)
        library = original / 'libfixture_consumer.dylib'
        subprocess.run(['xcrun', 'clang', '-dynamiclib', original / 'consumer.c', dependency, '-o', library,
                        '-Wl,-headerpad_max_install_names', '-install_name', str(library)], check=True, timeout=60)
        for path in [dependency, library]:
            shutil.copy2(path, frameworks / path.name)
        script = (ROOT / 'Mirage/scripts/bundle_renderers.sh').read_text()
        functions = []
        for name in ['is_bundleable', 'resolve', 'remove_build_rpaths', 'retarget_lib']:
            match = re.search(r'^' + name + r'\(\) \{\n.*?^\}', script, re.MULTILINE | re.DOTALL)
            self.assertIsNotNone(match, name)
            functions.append(match.group())
        shell = 'set -euo pipefail\n' + '\n'.join(functions) + '\nFRAMEWORKS="$1"\nretarget_lib "$2"\n'
        consumer = frameworks / library.name
        subprocess.run(['bash', '-c', shell, 'retarget-test', frameworks, consumer], check=True, timeout=30)
        shutil.rmtree(original)
        for path in frameworks.glob('*.dylib'):
            subprocess.run(['codesign', '--force', '--sign', '-', path], check=True, capture_output=True, timeout=20)
        environment = {key: value for key, value in os.environ.items() if not key.startswith('DYLD_')}
        probe = 'import ctypes,sys; lib=ctypes.CDLL(sys.argv[1]); assert lib.fixture_read() == 42'
        subprocess.run(['/usr/bin/python3', '-c', probe, consumer], env=environment, check=True, timeout=20)

    def test_codec_provenance_rejects_external_library(self):
        libraries = self.root / 'bundled'
        libraries.mkdir()
        dyld = Mock()
        dyld._dyld_image_count.return_value = 3
        images = [libraries / 'libavcodec.1.dylib', libraries / 'libavformat.1.dylib',
                  self.root / 'outside/libavutil.1.dylib']
        dyld._dyld_get_image_name.side_effect = [os.fsencode(path) for path in images]
        with patch.object(CODECS.ctypes, 'CDLL', return_value=dyld):
            with self.assertRaisesRegex(RuntimeError, 'external dependency'):
                CODECS.verify_loaded_codec_paths(libraries)
        dyld._dyld_get_image_name.side_effect = [os.fsencode(libraries / path.name) for path in images]
        with patch.object(CODECS.ctypes, 'CDLL', return_value=dyld):
            CODECS.verify_loaded_codec_paths(libraries)

    def test_component_lookup(self):
        binary = self.root / 'lookup'
        subprocess.run(['xcrun', 'swiftc', ROOT / 'Mirage/Mirage Screen Saver/MirageHostApplication.swift',
                        ROOT / 'Mirage/Tests/SharedRuntimeRegression.swift', '-o', binary], check=True, timeout=90)
        subprocess.run([binary, self.root / 'lookup-fixtures'], check=True, timeout=20)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--app', type=Path)
    args, rest = parser.parse_known_args()
    APP_PATH = args.app
    unittest.main(argv=[sys.argv[0], *rest])
