#!/usr/bin/env python3
#
#  run_editor_installation.py
#  Mirage Wallpaper
#
#  Created on 2026/10/28.
#  Copyright © 2026 王孝慈. All rights reserved.
#

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--assemble-root', type=Path, help='Isolated test root with unmodified WE files in downloads/.')
parser.add_argument('--support', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='mirage-editor-tests-') as temp:
    directory = Path(temp)
    executable = directory / 'tests'
    subprocess.run(['swiftc', '-D', 'MIRAGE_EDITOR_TESTING',
                    str(root / 'Mirage/Mirage Wallpaper/Services/EditorInstallation.swift'),
                    str(root / 'Mirage/Tests/EditorInstallationTests.swift'), '-o', str(executable)], check=True)
    environment = dict(os.environ, MIRAGE_EDITOR_TEST_ROOT=str(args.assemble_root or directory / 'Editor'),
                       MIRAGE_EDITOR_TEST_SUPPORT=str(args.support or root / 'StandaloneEditor/dist/EditorSupport'))
    subprocess.run([str(executable)] + (['--assemble'] if args.assemble_root else []), env=environment, check=True)
