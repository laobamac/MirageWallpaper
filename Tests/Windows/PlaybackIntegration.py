import argparse
import json
import math
import os
from pathlib import Path
import queue
import struct
import subprocess
import tempfile
import threading
import time
import uuid
import zlib


def chunk(name, data):
    return name + struct.pack('<I', len(data)) + data + (b'\0' if len(data) % 2 else b'')


def make_video(path):
    width, height, fps, frames = 32, 32, 10, 30
    frame_size = width * height * 3
    sample_rate, channels, sample_bits = 48000, 2, 16
    block_align = channels * sample_bits // 8
    audio_chunk_frames = sample_rate // fps
    audio_chunk_bytes = audio_chunk_frames * block_align
    audio_bytes_per_second = sample_rate * block_align
    avih = struct.pack('<14I', 1000000 // fps, frame_size * fps + audio_bytes_per_second,
                       0, 0x10, frames, 0, 2, max(frame_size, audio_chunk_bytes),
                       width, height, 0, 0, 0, 0)
    video_strh = struct.pack('<4s4sIHH8I4h', b'vids', b'DIB ', 0, 0, 0,
                             0, 1, fps, 0, frames, frame_size, 0xffffffff, 0,
                             0, 0, width, height)
    video_strf = struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0,
                             frame_size, 0, 0, 0, 0)
    audio_frames = audio_chunk_frames * frames
    audio_strh = struct.pack('<4s4sIHH8I4h', b'auds', b'\\0' * 4, 0, 0, 0,
                             0, 1, sample_rate, 0, audio_frames, audio_chunk_bytes,
                             0xffffffff, block_align, 0, 0, 0, 0)
    audio_strf = struct.pack('<HHIIHHH', 1, channels, sample_rate, audio_bytes_per_second,
                             block_align, sample_bits, 0)
    video_stream = chunk(b'LIST', b'strl' + chunk(b'strh', video_strh) + chunk(b'strf', video_strf))
    audio_stream = chunk(b'LIST', b'strl' + chunk(b'strh', audio_strh) + chunk(b'strf', audio_strf))
    header = chunk(b'LIST', b'hdrl' + chunk(b'avih', avih) + video_stream + audio_stream)
    pixels = b'\0\0\xff' * (width * height)
    media = []
    for frame in range(frames):
        media.append(chunk(b'00db', pixels))
        samples = bytearray()
        for sample in range(audio_chunk_frames):
            phase = 2.0 * 3.141592653589793 * 440.0 * (frame * audio_chunk_frames + sample) / sample_rate
            value = int(400 * math.sin(phase))
            samples.extend(struct.pack('<hh', value, value))
        media.append(chunk(b'01wb', samples))
    body = b'AVI ' + header + chunk(b'LIST', b'movi' + b''.join(media))
    path.write_bytes(chunk(b'RIFF', body))


def png_center_rgb(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise RuntimeError('snapshot is not a PNG')
    width = height = depth = color = interlace = 0
    compressed = bytearray()
    offset = 8
    while offset + 12 <= len(data):
        size = struct.unpack_from('>I', data, offset)[0]
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + size]
        offset += size + 12
        if kind == b'IHDR':
            width, height, depth, color, _, _, interlace = struct.unpack('>IIBBBBB', payload)
        elif kind == b'IDAT':
            compressed.extend(payload)
        elif kind == b'IEND':
            break
    channels = {2: 3, 6: 4}.get(color)
    if not width or not height or depth != 8 or channels is None or interlace != 0:
        raise RuntimeError('unsupported PNG snapshot format')
    scanlines = zlib.decompress(compressed)
    stride = width * channels
    if len(scanlines) != height * (stride + 1):
        raise RuntimeError('invalid PNG scanline data')
    previous = bytearray(stride)
    rows = []
    cursor = 0

    def paeth(left, above, upper_left):
        prediction = left + above - upper_left
        distances = (abs(prediction - left), abs(prediction - above), abs(prediction - upper_left))
        return (left, above, upper_left)[distances.index(min(distances))]

    for _ in range(height):
        filter_kind = scanlines[cursor]
        cursor += 1
        row = bytearray(scanlines[cursor:cursor + stride])
        cursor += stride
        for index in range(stride):
            left = row[index - channels] if index >= channels else 0
            above = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if filter_kind == 1:
                row[index] = (row[index] + left) & 0xff
            elif filter_kind == 2:
                row[index] = (row[index] + above) & 0xff
            elif filter_kind == 3:
                row[index] = (row[index] + (left + above) // 2) & 0xff
            elif filter_kind == 4:
                row[index] = (row[index] + paeth(left, above, upper_left)) & 0xff
            elif filter_kind != 0:
                raise RuntimeError('invalid PNG filter')
        rows.append(row)
        previous = row
    pixel = rows[height // 2][(width // 2) * channels:(width // 2) * channels + 3]
    return tuple(pixel)


def make_fixtures(root):
    fixtures = {}
    for kind in ('scene', 'web', 'video'):
        directory = root / ('测试 ' + kind)
        directory.mkdir()
        fixtures[kind] = directory
        filename = {'scene': 'scene.json', 'web': 'index.html', 'video': 'sample.avi'}[kind]
        (directory / 'project.json').write_text(json.dumps({
            'title': 'Windows 回归 ' + kind, 'type': kind, 'file': filename,
            'general': {'properties': {'green': {'type': 'bool', 'value': False}}}
        }), encoding='utf-8')
    (fixtures['scene'] / 'scene.json').write_text(json.dumps({
        'camera': {}, 'general': {'clearcolor': [1, 0, 0],
                                  'orthogonalprojection': {'width': 960, 'height': 540}},
        'objects': [{'id': 1, 'image': 'models/util/solidlayer.json',
                     'origin': [480, 270, 0], 'size': [960, 540],
                     'color': [1, 0, 0], 'visible': True}]
    }), encoding='utf-8')
    (fixtures['web'] / 'index.html').write_text('''<!doctype html>
<html><head><meta charset="utf-8"><style>html,body{margin:0;width:100%;height:100%;background:#f00}</style></head>
<body><script>window.wallpaperPropertyListener={applyUserProperties:function(p){
if(p.green)document.body.style.background=p.green.value?'#00ff00':'#ff0000';
}};</script></body></html>''', encoding='utf-8')
    make_video(fixtures['video'] / 'sample.avi')
    return fixtures


class Renderer:
    def __init__(self, args, environment):
        self.process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, env=environment)
        self.events = queue.Queue()
        self.logs = []
        self.history = []
        self.threads = [threading.Thread(target=self.read, args=(self.process.stdout, True), daemon=True),
                        threading.Thread(target=self.read, args=(self.process.stderr, False), daemon=True)]
        for thread in self.threads:
            thread.start()

    def read(self, stream, events):
        for raw in iter(stream.readline, b''):
            text = raw.decode('utf-8', errors='replace').strip()
            if events:
                try:
                    value = json.loads(text)
                    if isinstance(value, dict):
                        self.history.append(value)
                        self.events.put(value)
                        continue
                except json.JSONDecodeError:
                    pass
            self.logs.append(text)

    def send(self, value):
        self.process.stdin.write((json.dumps(value) + '\n').encode('utf-8'))
        self.process.stdin.flush()

    def wait(self, predicate, timeout=45):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                event = self.events.get(timeout=min(0.25, max(0.01, end - time.monotonic())))
            except queue.Empty:
                if self.process.poll() is not None:
                    break
                continue
            if event.get('event') == 'error':
                raise RuntimeError(json.dumps(event))
            if predicate(event):
                return event
        raise RuntimeError(f'event timeout/exit {self.process.poll()}; '
                           f'events={self.history}; logs={self.logs[-30:]}')

    def snapshot(self, path):
        token = str(uuid.uuid4())
        self.send({'cmd': 'snapshot', 'path': str(path), 'token': token})
        event = self.wait(lambda e: e.get('event') == 'snapshot-done' and e.get('token') == token)
        if event.get('ok', event.get('success')) is not True:
            raise RuntimeError(f'snapshot failed: {event}')
        data = path.read_bytes()
        if len(data) < 32:
            raise RuntimeError('empty PNG snapshot')
        return png_center_rgb(data)

    def close(self):
        if self.process.poll() is None:
            try:
                self.send({'cmd': 'quit'})
                self.process.wait(timeout=8)
            except (OSError, ValueError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait(timeout=5)
        for thread in self.threads:
            thread.join(timeout=2)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()


def run_kind(kind, binary, directory, assets, environment, output, load_from_memory=False):
    args = [str(binary)]
    if kind == 'scene':
        args += [str(assets), str(directory / 'scene.json')]
    else:
        args += [str(directory)]
    args += ['--screen', '0', '--muted', '--run-seconds', '90', '--control-stdin']
    if load_from_memory:
        args.append('--load-from-memory')
    suffix = '-memory' if load_from_memory else ''
    renderer = Renderer(args, environment)
    try:
        renderer.wait(lambda e: e.get('event') == 'first-frame-presented')
        for command in ({'cmd': 'pause'}, {'cmd': 'volume', 'value': 0.2},
                        {'cmd': 'muted', 'value': True}, {'cmd': 'fps', 'value': 20},
                        {'cmd': 'fillmode', 'value': 'contain'}, {'cmd': 'resume'}):
            renderer.send(command)
        before = renderer.snapshot(output / (kind + suffix + '.png'))
        if before[0] < 200 or before[1] > 60 or before[2] > 60:
            raise RuntimeError(f'{kind} center pixel was not red: {before}')
        if kind == 'web':
            renderer.send({'cmd': 'setProperty', 'key': 'green', 'type': 'bool', 'value': True})
            after = renderer.snapshot(output / ('web-property' + suffix + '.png'))
            if after[1] < 200 or after[0] > 60 or after[2] > 60:
                raise RuntimeError(f'web user property did not change center pixel to green: {after}')
        renderer.process.stdin.close()
        renderer.process.wait(timeout=8)
        for thread in renderer.threads:
            thread.join(timeout=2)
        if renderer.process.returncode != 0:
            raise RuntimeError(f'EOF exit code {renderer.process.returncode}')
        if kind == 'video' and not any('AO: [wasapi]' in line for line in renderer.logs):
            raise RuntimeError(f'video audio did not initialize WASAPI output: {renderer.logs[-30:]}')
        print(json.dumps({'renderer': kind, 'load_from_memory': load_from_memory,
                          'result': 'PASS', 'events': renderer.history},
                         ensure_ascii=False), flush=True)
    finally:
        renderer.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, required=True)
    parser.add_argument('--assets-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--kind', choices=('scene', 'web', 'video', 'all'), default='all')
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment['PATH'] = str(args.bin_dir) + os.pathsep + environment.get('PATH', '')
    failures = []
    with tempfile.TemporaryDirectory(prefix='mirage-render-test-') as temporary:
        fixtures = make_fixtures(Path(temporary))
        kinds = ('scene', 'web', 'video') if args.kind == 'all' else (args.kind,)
        for kind in kinds:
            binary = args.bin_dir / (kind.capitalize() + 'Wallpaper.exe')
            try:
                for load_from_memory in (False, True):
                    run_kind(kind, binary, fixtures[kind], args.assets_dir,
                             environment, args.output_dir, load_from_memory)
            except Exception as error:
                failures.append(kind)
                print(json.dumps({'renderer': kind, 'result': 'FAIL', 'error': str(error)},
                                 ensure_ascii=False), flush=True)
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
