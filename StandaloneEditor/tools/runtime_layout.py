#!/usr/bin/env python3
# Copyright © 2026 王孝慈. All rights reserved.
"""Audit and relocate the native libraries in an editor support payload."""

import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess

MACHO_MAGICS = {b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'}


def output(*args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def is_macho(path):
    if path.is_symlink() or not path.is_file():
        return False
    with path.open('rb') as stream:
        return stream.read(4) in MACHO_MAGICS


def dependencies(path):
    return re.findall(r'^\s+(.+?) \(compatibility version ', output('otool', '-L', path), re.M)


def system_library(path):
    return path.startswith(('/usr/lib/', '/System/Library/'))


def audit(root, architecture):
    root = Path(root).resolve()
    count = 0
    for path in sorted(root.rglob('*')):
        if path.is_symlink():
            if os.path.isabs(os.readlink(path)) or not path.resolve().is_relative_to(root) or not path.exists():
                raise ValueError('Invalid runtime symlink: ' + str(path.relative_to(root)))
        elif is_macho(path):
            count += 1
            if output('lipo', '-archs', path).split() != [architecture]:
                raise ValueError('Unexpected native architecture: ' + str(path.relative_to(root)))
            identity = output('otool', '-D', path).splitlines()[1:]
            for dep in dependencies(path):
                if dep in identity or system_library(dep):
                    continue
                if not dep.startswith('@loader_path/'):
                    raise ValueError(f'Non-portable dependency: {path}: {dep}')
                target = path.parent / dep.removeprefix('@loader_path/')
                if not target.exists() or not target.resolve().is_relative_to(root):
                    raise ValueError(f'Missing packaged dependency: {path}: {dep}')
    if not count:
        raise ValueError('The runtime contains no native binaries.')
    return count


def make_portable(root, extra_libraries=()):
    """Copy external dependencies and replace references with loader-relative paths."""
    root = Path(root).resolve()
    external = root / 'lib/external'
    external.mkdir(parents=True, exist_ok=True)
    origins = {}

    def copy(source):
        source = Path(source).resolve()
        target = external / source.name
        if target in origins:
            if origins[target] != source and digest(origins[target]) != digest(source):
                raise ValueError('Conflicting dependency names: ' + source.name)
        else:
            shutil.copy2(source, target)
            origins[target] = source
        return target

    for library in extra_libraries:
        copy(library)
    pending = [p for p in root.rglob('*') if is_macho(p)]
    processed = set()
    while pending:
        path = pending.pop()
        if path in processed:
            continue
        processed.add(path)
        source = origins.get(path, path)
        identity = output('otool', '-D', path).splitlines()[1:]
        rpaths = re.findall(r'cmd LC_RPATH\n\s+cmdsize \d+\n\s+path (.+?) \(offset', output('otool', '-l', source))
        for dep in dependencies(path):
            if dep in identity or system_library(dep):
                continue
            if dep.startswith('@loader_path/'):
                resolved = source.parent / dep.removeprefix('@loader_path/')
            elif dep.startswith('@rpath/'):
                candidates = [Path(p.replace('@loader_path', str(source.parent))) / dep.removeprefix('@rpath/') for p in rpaths]
                resolved = next((p for p in candidates if p.exists()), None)
                if resolved is None:
                    raise ValueError(f'Cannot resolve {dep} from {source}')
            elif dep.startswith('/'):
                resolved = Path(dep)
            else:
                raise ValueError(f'Cannot relocate dependency: {source}: {dep}')
            resolved = resolved.resolve()
            target = resolved if resolved.is_relative_to(root) else copy(resolved)
            relative = '@loader_path/' + os.path.relpath(target, path.parent)
            subprocess.run(['install_name_tool', '-change', dep, relative, str(path)], check=True, capture_output=True)
            pending.append(target)
        for rpath in rpaths:
            if rpath.startswith('/') and not system_library(rpath):
                subprocess.run(['install_name_tool', '-delete_rpath', rpath, str(path)], check=True, capture_output=True)
    return origins
