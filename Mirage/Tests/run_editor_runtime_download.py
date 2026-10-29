#!/usr/bin/env python3
# Copyright © 2026 王孝慈. All rights reserved.
"""Exercise runtime downloads with deterministic URLSession responses and real archives."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--archive', type=Path, help='Optional existing local archive for a full offline payload check.')
parser.add_argument('--manifest', type=Path)
args = parser.parse_args()
if bool(args.archive) != bool(args.manifest):
    parser.error('--archive and --manifest must be supplied together')
with tempfile.TemporaryDirectory(prefix='mirage-runtime-download-tests-') as temporary:
    root = Path(temporary)
    executable = root / 'tests'
    subprocess.run(['swiftc', '-swift-version', '6', '-D', 'MIRAGE_EDITOR_TESTING',
                    str(ROOT / 'Mirage/Mirage Wallpaper/Services/EditorInstallation.swift'),
                    str(ROOT / 'Mirage/Mirage Wallpaper/Services/EditorRuntimeDownload.swift'),
                    str(ROOT / 'Mirage/Tests/EditorRuntimeDownloadTests.swift'), '-o', str(executable)], check=True)
    environment = dict(os.environ, MIRAGE_EDITOR_TEST_ROOT=str(root / 'Editor'),
                       MIRAGE_EDITOR_TEST_SUPPORT=str(root / 'support'))
    if args.archive:
        environment.update(MIRAGE_RUNTIME_TEST_ARCHIVE=str(args.archive.resolve()), MIRAGE_RUNTIME_TEST_MANIFEST=str(args.manifest.resolve()))
    subprocess.run([str(executable)], env=environment, check=True, timeout=300 if args.archive else 60)
