__copyright__ = "Copyright © 2026 王孝慈. All rights reserved."

import argparse
import ctypes
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[2]


def run(command, **kwargs):
    result = subprocess.run([str(x) for x in command], timeout=120, **kwargs)
    if result.returncode and result.stderr:
        print(result.stderr.decode() if isinstance(result.stderr, bytes) else result.stderr, file=sys.stderr)
    result.check_returncode()
    return result


def verify_loader_paths(libraries):
    for pattern in ('libavcodec.*.*.*.dylib', 'libavformat.*.*.*.dylib', 'libavutil.*.*.*.dylib',
                    'libswresample.*.*.*.dylib', 'libswscale.*.*.*.dylib', 'libdav1d.*.dylib'):
        for path in libraries.glob(pattern):
            if path.is_symlink():
                continue
            linked = subprocess.check_output(['otool', '-L', str(path)], text=True).splitlines()[2:]
            for line in linked:
                dependency = line.strip().split(' (compatibility')[0]
                if dependency.startswith(('/usr/lib/', '/System/')):
                    continue
                if not dependency.startswith('@loader_path/'):
                    raise RuntimeError(f'Codec dependency does not resolve beside its loader: {path.name}: {dependency}')
                target = (path.parent / dependency[len('@loader_path/'):]).resolve()
                if not target.is_file() or not target.is_relative_to(libraries.resolve()):
                    raise RuntimeError(f'Codec dependency is missing or escapes its bundle: {path.name}: {dependency}')


def verify_loaded_codec_paths(libraries):
    dyld = ctypes.CDLL(None)
    dyld._dyld_image_count.argtypes = []
    dyld._dyld_image_count.restype = ctypes.c_uint32
    dyld._dyld_get_image_name.argtypes = [ctypes.c_uint32]
    dyld._dyld_get_image_name.restype = ctypes.c_char_p
    prefixes = ('libavcodec.', 'libavformat.', 'libavutil.', 'libswresample.', 'libswscale.', 'libdav1d.')
    loaded = set()
    for index in range(dyld._dyld_image_count()):
        name = dyld._dyld_get_image_name(index)
        if not name:
            continue
        path = Path(os.fsdecode(name)).resolve()
        if path.name.startswith(prefixes):
            if not path.is_relative_to(libraries.resolve()):
                raise RuntimeError(f'Codec test loaded an external dependency: {path}')
            loaded.add(path.name)
    if not any(name.startswith('libavcodec.') for name in loaded) or not any(name.startswith('libavformat.') for name in loaded):
        raise RuntimeError('Loaded codec images could not be verified')
    print(f'PASS: all {len(loaded)} loaded codec libraries originate inside the bundle', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--app', type=Path)
    args = parser.parse_args()
    prefix = ROOT / 'Mirage/build/ffmpeg' / platform.machine()
    libraries = args.app.resolve() / 'Contents/Extensions/MirageWallpaperExtension.appex/Contents/Frameworks' if args.app else prefix / 'lib'
    if args.app:
        verify_loader_paths(libraries)
    codec_path = next(p for p in libraries.glob('libavcodec.*.*.*.dylib') if not p.is_symlink())
    codec = ctypes.CDLL(str(codec_path))
    codec.av_codec_iterate.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    codec.av_codec_iterate.restype = ctypes.c_void_p
    codec.av_codec_is_encoder.argtypes = [ctypes.c_void_p]
    cursor = ctypes.c_void_p()
    count = 0
    while entry := codec.av_codec_iterate(ctypes.byref(cursor)):
        assert not codec.av_codec_is_encoder(entry), 'Unexpected encoder in bundled FFmpeg'
        count += 1
    assert count > 100, f'Built-in decoder compatibility unexpectedly restricted: {count}'
    codec.avcodec_configuration.restype = ctypes.c_char_p
    config = codec.avcodec_configuration().decode()
    assert '--disable-network' in config and '--disable-encoders' in config
    fmt = ctypes.CDLL(str(next(p for p in libraries.glob('libavformat.*.*.*.dylib') if not p.is_symlink())))
    if args.app:
        verify_loaded_codec_paths(libraries)
    fmt.avio_enum_protocols.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_int]
    fmt.avio_enum_protocols.restype = ctypes.c_char_p
    cursor = ctypes.c_void_p()
    protocols = []
    while name := fmt.avio_enum_protocols(ctypes.byref(cursor), 0):
        protocols.append(name.decode())
    assert protocols == ['file'], protocols
    print(f'{count} decoders; no encoders; file-only protocol', flush=True)
    with tempfile.TemporaryDirectory(prefix='mirage-codecs-') as temporary:
        directory = Path(temporary)
        probe = directory / 'decode'
        flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libavformat', 'libavcodec', 'libavutil'],
                                        env={**os.environ, 'PKG_CONFIG_PATH': str(prefix / 'lib/pkgconfig')}, text=True)
        run(['xcrun', 'clang', ROOT / 'Mirage/Tests/CodecDecodeRegression.c', *shlex.split(flags), '-Wl,-headerpad_max_install_names', '-o', probe])
        if args.app:
            linked = subprocess.check_output(['otool', '-L', probe], text=True).splitlines()[1:]
            for row in linked:
                path = row.strip().split(' (compatibility')[0]
                if path.startswith(str(prefix)):
                    real = Path(path).resolve().name
                    run(['install_name_tool', '-change', path, str(libraries / real), probe])
            run(['install_name_tool', '-add_rpath', str(libraries), probe])
            run(['codesign', '-f', '-s', '-', probe], capture_output=True)
        encoders = subprocess.check_output(['ffmpeg', '-hide_banner', '-encoders'], text=True, stderr=subprocess.DEVNULL)
        av1_encoder = 'libaom-av1' if 'libaom-av1' in encoders else 'libsvtav1'
        video = [('h264', 'libx264', 'mp4'), ('hevc', 'libx265', 'mp4'), ('vp8', 'libvpx', 'webm'),
                 ('vp9', 'libvpx-vp9', 'webm'), ('av1', av1_encoder, 'mkv'), ('prores', 'prores_ks', 'mov')]
        audio = [('aac', 'm4a'), ('libmp3lame', 'mp3'), ('flac', 'flac'), ('libopus', 'ogg'), ('pcm_s16le', 'wav')]
        for name, encoder, extension in video:
            sample = directory / f'{name}.{extension}'
            command = ['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=128x128:rate=10',
                       '-t', '0.3', '-c:v', encoder, '-threads', '2']
            if encoder == 'libx265': command += ['-x265-params', 'pools=1:log-level=error']
            if encoder == 'libaom-av1': command += ['-cpu-used', '8']
            if encoder == 'libsvtav1': command += ['-preset', '12', '-svtav1-params', 'lp=2']
            run([*command, sample], capture_output=True)
            run([probe, sample])
        for encoder, extension in audio:
            sample = directory / f'{encoder}.{extension}'
            run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
                 '-t', '0.3', '-c:a', encoder, sample], capture_output=True)
            run([probe, sample])
    print('PASS: codec decoding and bundled library constraints')


if __name__ == '__main__':
    main()
