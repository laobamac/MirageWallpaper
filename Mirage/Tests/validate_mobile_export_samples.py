"""Export every scene in a WE sample folder and compare MPKG structure with WE exports.

Usage: python3 validate_mobile_export_samples.py --binary /path/to/MirageMobileOptionsReview \
    --references /path/to/we --output /path/to/output
"""

import argparse
import json
import math
from pathlib import Path
import struct
import subprocess


def archive(path: Path):
    data = path.read_bytes()
    position = 0

    def word():
        nonlocal position
        value = struct.unpack_from("<I", data, position)[0]
        position += 4
        return value

    def take(size):
        nonlocal position
        value = data[position:position + size]
        position += size
        return value

    version = take(word()).decode("ascii")
    entries = [(take(word()).decode("utf-8"), word(), word()) for _ in range(word())]
    return version, {name: data[position + offset:position + offset + size]
                     for name, offset, size in entries}


def texture_layout(data: bytes):
    # TEXI0001 + TEXB0004 output. Ignore encoder-dependent compressed bytes.
    header = struct.unpack_from("<iIiiiii", data, 18)
    assert data[46:55] == b"TEXB0004\0"
    count, image_type, reserved = struct.unpack_from("<iii", data, 55)
    position = 67
    slots = []
    for _ in range(count):
        mip_count = struct.unpack_from("<i", data, position)[0]
        position += 4
        mips = []
        for _ in range(mip_count):
            width, height, _compressed, raw_size, size = struct.unpack_from("<iiiii", data, position)
            position += 20 + size
            mips.append((width, height, raw_size))
        slots.append(mips)
    return header, image_type, reserved, slots, data[position:]


def json_equivalent(actual, reference):
    if isinstance(actual, bool) or isinstance(reference, bool):
        return actual is reference
    if isinstance(actual, (int, float)) and isinstance(reference, (int, float)):
        return math.isclose(actual, reference, rel_tol=1e-6, abs_tol=1e-6)
    if isinstance(actual, dict) and isinstance(reference, dict):
        return actual.keys() == reference.keys() and all(
            json_equivalent(actual[key], reference[key]) for key in actual)
    if isinstance(actual, list) and isinstance(reference, list):
        return len(actual) == len(reference) and all(
            json_equivalent(left, right) for left, right in zip(actual, reference))
    return actual == reference


def verify(actual_path: Path, reference_path: Path):
    actual_version, actual = archive(actual_path)
    reference_version, reference = archive(reference_path)
    assert actual_version == reference_version, (actual_version, reference_version)
    assert actual.keys() == reference.keys(), (
        sorted(actual.keys() - reference.keys()), sorted(reference.keys() - actual.keys()))
    textures = 0
    for name, payload in actual.items():
        expected = reference[name]
        if name.endswith(".tex"):
            assert payload == expected or texture_layout(payload) == texture_layout(expected), name
            textures += 1
        elif name == "scene.json" or name == "project.json":
            assert json_equivalent(json.loads(payload), json.loads(expected)), name
        elif not name.endswith((".frag", ".vert")):
            assert payload == expected, name
    return dict(version=actual_version, entries=len(actual), textures=textures,
                bytes=actual_path.stat().st_size)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--references", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.references = args.references.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    groups = (path for path in args.references.iterdir() if path.is_dir() and path.name.isdigit())
    for group in sorted(groups, key=lambda path: int(path.name)):
        source = next(group.glob("*/scene.pkg")).parent
        workshop_id = source.name
        qualities = [
            ("high", 2, "性能不优化"),
            ("balanced", 4, "高不优化"),
        ]
        if group.name in {"3", "5"}:
            qualities.append(("original", 1, "最高不优化"))
        for quality, factor, reference_prefix in qualities:
            reference = next(path for path in (group / "自定义").glob("*.mpkg")
                             if path.name.removeprefix("自定义").startswith(reference_prefix))
            output = args.output / f"{workshop_id}-{quality}.mpkg"
            subprocess.run([str(args.binary.resolve()), "--export", str(source), str(output),
                            str(factor), "false"], check=True, timeout=600)
            result = verify(output, reference)
            result.update(group=group.name, quality=quality, file=str(output))
            results.append(result)
            print(f"PASS {group.name} {quality}: {result['textures']} textures, "
                  f"{result['bytes'] / 1024 / 1024:.1f} MiB", flush=True)
    (args.output / "validation.json").write_text(json.dumps(results, indent=2, ensure_ascii=False) + "\n")
    print(f"Validated {len(results)} exports; manifest: {args.output / 'validation.json'}")


if __name__ == "__main__":
    main()
