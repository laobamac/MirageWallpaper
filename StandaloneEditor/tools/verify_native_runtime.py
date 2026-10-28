#!/usr/bin/env python3
# Copyright © 2026 王孝慈. All rights reserved.
"""Run owned CPU/GPU probes against the signed, relocated editor runtime."""

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

from runtime_layout import audit

PROJECT = Path(__file__).resolve().parents[1]


def verify(support, compiler, evidence):
    support = support.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    (evidence / 'result.json').unlink(missing_ok=True)
    catalog = json.loads((support / 'catalog.json').read_text())
    architecture = catalog['architecture']
    if platform.machine() != architecture:
        raise ValueError('Run validation natively on the payload architecture.')
    if subprocess.run(['sysctl', '-n', 'sysctl.proc_translated'], capture_output=True, text=True).stdout.strip() == '1':
        raise ValueError('Translated validation is not permitted.')
    native_files = audit(support, architecture)
    runtime = support / 'runtime'
    engine = runtime / 'Mirage WE Editor.app/Contents/Resources/wine'
    for source, target, libraries in [('WindowsX64Probe.c', 'cpu.exe', []),
                                       ('WindowsD3D11Probe.c', 'gpu.exe', ['-ld3d11', '-ldxguid'])]:
        if compiler:
            command = [str(compiler), '-O2', str(PROJECT / 'Tests' / source), '-o', str(evidence / target)] + libraries
        elif architecture == 'arm64':
            command = [str(PROJECT / 'build/native-arm64/hadron/toolchains/llvm-mingw/bin/x86_64-w64-mingw32-clang'),
                       '-O2', str(PROJECT / 'Tests' / source), '-o', str(evidence / target)] + libraries
        else:
            from build_runtime import existing
            mingw = Path(existing([os.environ.get('MIRAGE_MINGW_ROOT')] + list(Path('/usr/local/Cellar/mingw-w64').glob('*/toolchain-x86_64'))))
            llvm = existing([os.environ.get('MIRAGE_CLANG'), '/usr/local/opt/llvm/bin/clang'])
            obj = evidence / (target + '.o')
            subprocess.run([llvm, '--target=x86_64-w64-windows-gnu', '--sysroot=' + str(mingw / 'x86_64-w64-mingw32'),
                            '-O2', '-c', str(PROJECT / 'Tests' / source), '-o', str(obj)], check=True)
            command = [str(mingw / 'bin/x86_64-w64-mingw32-gcc'), str(obj), '-o', str(evidence / target)] + libraries
        subprocess.run(command, check=True)
    with tempfile.TemporaryDirectory(prefix='mirage-runtime-probe-') as temporary:
        prefix = Path(temporary) / 'prefix'
        environment = dict(os.environ, WINEPREFIX=str(prefix), WINEARCH='win64', WINEDEBUG='-all',
                           WINEDLLOVERRIDES='winemenubuilder.exe=d',
                           PATH=str(engine / 'bin') + ':/usr/bin:/bin:/usr/sbin:/sbin')
        environment.pop('DYLD_INSERT_LIBRARIES', None)
        environment.pop('WINELOADER', None)
        if architecture == 'arm64':
            environment['DYLD_LIBRARY_PATH'] = str(engine / 'lib/external')
        unix = 'aarch64-unix' if architecture == 'arm64' else 'x86_64-unix'
        environment['VK_DRIVER_FILES'] = str(engine / 'lib/wine' / unix / 'vulkan/icd.d/MoltenVK_icd.json')
        def run(command, log_name, marker=None):
            log = evidence / log_name
            with log.open('w') as stream:
                result = subprocess.run(list(map(str, command)), env=environment, stdout=stream, stderr=subprocess.STDOUT, timeout=240)
            content = log.read_text(errors='replace')
            if result.returncode or (marker and marker not in content):
                raise RuntimeError(f'{log_name} failed (exit {result.returncode}):\n' + content[-4000:])
            print(log_name + ': PASS', flush=True)
        try:
            run([engine / 'bin/wineboot', '--init'], 'wineboot.log')
            run([engine / 'bin/wine', evidence / 'cpu.exe'], 'windows-x64.log', 'MIRAGE_FEX_X64_OK')
            for name in ['d3d11.dll', 'd3d10core.dll', 'mirage_dxvk.dll']:
                target = prefix / 'drive_c/windows/system32' / name
                if target.exists() or target.is_symlink(): target.unlink()
                shutil.copy2(runtime / 'overrides' / name, target)
            environment.update(WINEDLLOVERRIDES='winemenubuilder.exe=d;d3d11,d3d10core=n,b;dxgi,vulkan-1=b',
                               DYLD_INSERT_LIBRARIES=str(runtime / 'MetalInterop.dylib'),
                               WINELOADER=str(runtime / 'Mirage WE Editor.app/Contents/MacOS/wine'),
                               DXVK_CONFIG_FILE='Z:' + str(runtime / 'dxvk.conf'),
                               DXVK_LOG_PATH=str(evidence), DXVK_STATE_CACHE_PATH=str(evidence),
                               DXVK_LOG_LEVEL='info', MVK_CONFIG_LOG_LEVEL='1')
            run([runtime / 'Mirage WE Editor.app/Contents/MacOS/wine', evidence / 'gpu.exe'],
                'windows-d3d11.log', 'MIRAGE_D3D11_SHARED_TEXTURE_OK')
        finally:
            environment.pop('DYLD_INSERT_LIBRARIES', None)
            for arguments in [['-k'], ['-w']]:
                subprocess.run([str(engine / 'bin/wineserver')] + arguments, env=environment,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20)
    (evidence / 'result.json').write_text(json.dumps(dict(architecture=architecture, rosetta=False,
        nativeFiles=native_files, x64Execution='passed', sharedTextureReadback='passed'), indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--support', type=Path, required=True)
    parser.add_argument('--compiler', type=Path)
    parser.add_argument('--evidence', type=Path, default=PROJECT / 'build/native-arm64/packaged-validation')
    args = parser.parse_args()
    verify(args.support, args.compiler, args.evidence.resolve())
