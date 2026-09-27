"""Verify decoded RGBA pixels through complete exports, including PNG fallback formats.

No Pillow dependency: PNGs are generated from known samples and MPKG LZ4
payloads are decoded with macOS libcompression. Run with --binary and --output.
"""

import argparse
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import zlib

from validate_mobile_export_samples import archive


def word(value):
    return struct.pack("<I", value & 0xffffffff)


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(width, height, samples, channels, depth=8, palette=None, transparent=None, interlaced=False):
    color_type = 3 if palette else 6 if channels == 4 else 2
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth,
                                                            color_type, 0, 0, int(interlaced)))
    if palette:
        data += chunk(b"PLTE", palette)
    if transparent:
        data += chunk(b"tRNS", transparent)
    stride = channels * (depth // 8)
    passes = [(0, 0, 1, 1)] if not interlaced else [
        (0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),
        (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
    scanlines = bytearray()
    for x0, y0, dx, dy in passes:
        if x0 >= width or y0 >= height:
            continue
        for y in range(y0, height, dy):
            scanlines.append(0)
            for x in range(x0, width, dx):
                offset = (y * width + x) * stride
                scanlines += samples[offset:offset + stride]
    return data + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b"")


def source_package(entries):
    data = word(8) + b"PKGV0024" + word(len(entries))
    offset = 0
    for name, content in entries:
        name = name.encode()
        data += word(len(name)) + name + word(offset) + word(len(content))
        offset += len(content)
    return data + b"".join(content for _, content in entries)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expect-native", action="store_true", help="Require native PNG routes in a Debug build")
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    lib = ctypes.CDLL("/usr/lib/libcompression.dylib")
    lib.compression_decode_buffer.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                              ctypes.c_size_t, ctypes.c_void_p, ctypes.c_int]
    lib.compression_decode_buffer.restype = ctypes.c_size_t
    width, height = 260, 132
    colors = [(63, 127, 201, 0), (10, 240, 77, 17), (250, 1, 33, 128), (3, 190, 99, 255)]
    rgba = bytes(v for i in range(width * height) for v in colors[(i + i // width) % 4])
    rgb = bytes(v for i in range(width * height) for v in colors[(i + i // width) % 4][:3])
    opaque = bytes(v for i in range(width * height) for v in (*colors[(i + i // width) % 4][:3], 255))
    palette = bytes(v for c in colors for v in c[:3])
    indices = bytes((i + i // width) % 4 for i in range(width * height))
    # Pin FFmpeg's existing rounded 16->8 conversion for the fallback path.
    rgba16 = b"".join(struct.pack(">H", value * 257) for value in rgba)
    rounded16 = bytes(min(255, (value * 257 + 128) >> 8) for value in rgba)
    trns_expected = bytes(v for i in range(width * height)
                          for v in (*colors[(i + i // width) % 4][:3], 0 if (i + i // width) % 4 == 0 else 255))
    fixtures = [
        ("rgba8", png(width, height, rgba, 4), rgba, width, height),
        ("rgb8", png(width, height, rgb, 3), opaque, width, height),
        ("cropped-rgba8", png(width, height, rgba, 4), rgba, 259, 129),
        ("cropped-rgb8", png(width, height, rgb, 3), opaque, 257, 130),
        ("rgba16", png(width, height, rgba16, 4, depth=16), rounded16, width, height),
        ("indexed", png(width, height, indices, 1, palette=palette,
                        transparent=bytes(c[3] for c in colors)), rgba, width, height),
        ("rgb-transparent-key", png(width, height, rgb, 3,
                                    transparent=struct.pack(">HHH", *colors[0][:3])), trns_expected, width, height),
        ("interlaced", png(width, height, rgba, 4, interlaced=True), rgba, width, height),
    ]
    results = []
    for name, image, pixels, map_width, map_height in fixtures:
        fixture = root / name
        fixture.mkdir(exist_ok=True)
        (fixture / "project.json").write_text(json.dumps(dict(file="scene.json", preview="preview.png",
                                                             title=name, type="scene")))
        (fixture / "preview.png").write_bytes(image)
        tex = b"TEXV0005\0TEXI0001\0" + b"".join(word(v) for v in [0, 2, width, height, map_width, map_height, 0])
        tex += b"TEXB0004\0" + b"".join(word(v) for v in [1, 13, 0, 1, width, height, 0, 0, len(image)]) + image
        (fixture / "scene.pkg").write_bytes(source_package([
            ("scene.json", b'{"general":{},"objects":[]}'), ("materials/image.tex", tex)]))
        output = fixture / "export.mpkg"
        result = subprocess.run([str(args.binary.resolve()), "--export", str(fixture), str(output), "1", "true"],
                                env=dict(os.environ, MIRAGE_SCENE_EXPORT_TIMING="1"),
                                check=True, capture_output=True, text=True, timeout=60)
        data = archive(output)[1]["materials/image.tex"]
        fmt = struct.unpack_from("<i", data, 18)[0]
        w, h, compressed, raw_size, size = struct.unpack_from("<iiiii", data, 71)
        assert (fmt, w, h, raw_size) == (0, map_width, map_height, map_width * map_height * 4), name
        payload = data[91:91 + size]
        if compressed:
            buffer = ctypes.create_string_buffer(raw_size)
            decoded = lib.compression_decode_buffer(buffer, raw_size, payload, len(payload), None, 0x101)
            assert decoded == raw_size, name
            payload = buffer.raw
        expected = b"".join(pixels[y * width * 4:(y * width + map_width) * 4] for y in range(map_height))
        assert payload == expected, f"Pixel values differ: {name}"
        calls = sum(line.startswith('MIRAGE_EXPORT_TIMING ') and '"tool":"ffmpeg"' in line
                    for line in result.stderr.splitlines())
        if args.expect_native:
            expected_calls = 0 if name in {"rgba8", "rgb8", "cropped-rgba8", "cropped-rgb8"} else 1
            assert calls == expected_calls, f"Unexpected decoder route: {name}: {calls} FFmpeg calls"
        results.append(dict(case=name, width=w, height=h, rgba_bytes=len(payload), ffmpeg_calls=calls, status="pass"))
        print(f"PASS {name}: {len(payload)} exact RGBA bytes", flush=True)
    (root / "pixels.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
