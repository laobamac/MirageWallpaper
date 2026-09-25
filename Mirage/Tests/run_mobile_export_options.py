"""Build and exercise the mobile exporter with real FFmpeg/ETC2 tools and synthetic scene packages."""
import argparse
from pathlib import Path
import platform
import json
import plistlib
import struct
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--derived-data', type=Path, required=True)
parser.add_argument('--tools', type=Path, required=True, help='Directory containing bundled ffmpeg and EtcTool')
parser.add_argument('--preview', action='store_true')
parser.add_argument('--references', type=Path, help='Optional read-only reference sample directory')
args = parser.parse_args()
project = Path(__file__).resolve().parents[1]
products = args.derived_data / 'Build/Products/Debug'
app = products / 'Mirage Wallpaper.app/Contents'
artifacts = Path(tempfile.mkdtemp(prefix='mirage-mobile-options-regression-'))
(artifacts / 'SceneMobileTools').symlink_to(args.tools.resolve(), target_is_directory=True)
for language in ('en', 'zh-Hans', 'zh-Hant'):
    (artifacts / f'{language}.lproj').symlink_to(project / f'Mirage Wallpaper/Resources/{language}.lproj')
if args.preview:
    preview = artifacts / 'Mirage Mobile Options Review.app'
    (preview / 'Contents/MacOS').mkdir(parents=True)
    resources = preview / 'Contents/Resources'
    resources.mkdir()
    (preview / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
        CFBundleIdentifier='cn.laobamac.Mirage.MobileOptionsReview',
        CFBundleName='Mirage Mobile Options Review', CFBundleExecutable='MirageMobileOptionsReview',
        CFBundlePackageType='APPL', NSPrincipalClass='NSApplication')))
    for item in artifacts.glob('*.lproj'):
        (resources / item.name).symlink_to(item.resolve())
    binary = preview / 'Contents/MacOS/MirageMobileOptionsReview'
else:
    binary = artifacts / 'MirageMobileOptionsReview'
subprocess.run([
    'xcrun', 'swiftc', '-parse-as-library', '-target', f'{platform.machine()}-apple-macos14.2',
    '-I', str(products), '-F', str(products), '-F', str(products / 'PackageFrameworks'),
    str(project / 'Tests/MobileExportOptionsRegression.swift'),
    str(app / 'MacOS/Mirage Wallpaper.debug.dylib'),
    '-Xlinker', '-rpath', '-Xlinker', str(app / 'MacOS'),
    '-Xlinker', '-rpath', '-Xlinker', str(app / 'Frameworks'),
    '-o', str(binary),
], check=True, timeout=180)
print(f'Artifacts: {artifacts}', flush=True)
subprocess.run([str(binary), *(['--preview'] if args.preview else [])], check=True,
               cwd=artifacts, timeout=900 if args.preview else 180)

if args.references and not args.preview:
    def unpack(path):
        data = path.read_bytes()
        pos = 0
        def u():
            nonlocal pos
            v = struct.unpack_from('<I', data, pos)[0]
            pos += 4
            return v
        def take(n):
            nonlocal pos
            v = data[pos:pos+n]
            pos += n
            return v
        take(u())
        entries = [(take(u()).decode(), u(), u()) for _ in range(u())]
        return {name: data[pos+offset:pos+offset+size] for name, offset, size in entries}

    def tex_layout(data):
        # Mobile TEXB0004 single-slot/single-mip layout; payload differs between encoders.
        fmt, flags, w, h, mw, mh, _ = struct.unpack_from('<iIiiiii', data, 18)
        slots, image, reserved, mips, pw, ph, compressed, raw, size = struct.unpack_from('<iiiiiiiii', data, 55)
        assert slots == 1 and mips == 1
        sprite = data[91+size:]
        return fmt, flags, (w,h,mw,mh), (pw,ph), raw, sprite

    selections = {
        '2': ['materials/LOGO.tex', 'materials/旗子-中景.tex'],
        '4': ['materials/rakete.tex', 'materials/effects/waterripplenormal.tex',
              'materials/workshop/3732231168/dayNightToggleSprite.tex'],
        '8': ['materials/workshop/2214863259/particle/matrix spritesheet 72.tex'],
    }
    for group, names in selections.items():
        folder = args.references / group
        source = unpack(next(folder.rglob('scene.pkg')))
        fixture = artifacts / ('reference-' + group)
        fixture.mkdir()
        project_data = dict(file='scene.json', preview='preview.png', title='Reference fixture', type='scene')
        (fixture / 'project.json').write_text(json.dumps(project_data))
        (fixture / 'preview.png').write_bytes(b'preview')
        entries = [('scene.json', b'{"general":{},"objects":[]}')] + [(name, source[name]) for name in names]
        output = bytearray(struct.pack('<I', 8) + b'PKGV0024' + struct.pack('<I', len(entries)))
        offset = 0
        for name, data in entries:
            encoded = name.encode()
            output += struct.pack('<I', len(encoded)) + encoded + struct.pack('<II', offset, len(data))
            offset += len(data)
        for _, data in entries:
            output += data
        (fixture / 'scene.pkg').write_bytes(output)
        for factor, prefix in [(1, '最高'), (2, '性能'), (4, '高')]:
            for pixel in (False, True):
                label = prefix + ('优化' if pixel else '不优化')
                reference_path = next(p for p in (folder / '自定义').glob('*.mpkg') if p.name.startswith(label))
                reference = unpack(reference_path)
                result_path = fixture / 'result.mpkg'
                subprocess.run([str(binary), '--export', str(fixture), str(result_path), str(factor), str(pixel).lower()],
                               check=True, timeout=120)
                actual = unpack(result_path)
                for name in names:
                    assert tex_layout(actual[name]) == tex_layout(reference[name]), (group, label, name,
                        tex_layout(actual[name])[:5], tex_layout(reference[name])[:5])
                print(f'PASS: reference group {group}, {label} ({len(names)} texture layouts)', flush=True)
