"""Measure complete scene exports and optionally require byte-identical baseline output.

Use a Debug app (ENABLE_CODE_COVERAGE=NO) with run_mobile_export_options.py to
build the --binary executable. Both runs must use the same app configuration,
FFmpeg and EtcTool builds, and the same machine. Tool worker times are summed
across concurrent tasks, so they are not additive wall-clock time.
"""

import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--references", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--matrix", action="store_true",
                        help="All three resolutions with pixel-art optimization both off and on")
    parser.add_argument("--groups", nargs="+", help="Limit measurement to these numbered sample folders")
    parser.add_argument("--qualities", nargs="+", help="Limit measurement to named qualities, e.g. high-pixel")
    parser.add_argument("--baseline-library", type=Path,
                        help="Directory with an earlier Debug dylib for interleaved comparisons")
    parser.add_argument("--candidate-library", type=Path,
                        help="Directory with the candidate Debug dylib (use with --baseline-library)")
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    if bool(args.baseline_library) != bool(args.candidate_library):
        parser.error("Both --baseline-library and --candidate-library are required for paired timing")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    baseline = json.loads(args.baseline.read_text())["cases"] if args.baseline else {}
    cases = {}
    env = dict(os.environ, MIRAGE_SCENE_EXPORT_TIMING="1",
               LLVM_PROFILE_FILE=str(args.output / "export-%p.profraw"))
    groups = sorted((p for p in args.references.resolve().iterdir()
                     if p.is_dir() and p.name.isdigit() and (not args.groups or p.name in args.groups)),
                    key=lambda p: int(p.name))
    if not groups:
        parser.error("No scene sample folders found")
    for run in range(args.runs):
        for group in groups:
            source = next(group.glob("*/scene.pkg")).parent
            qualities = [("high", 2, False), ("balanced", 4, False)]
            if args.matrix or group.name in {"3", "5"}:
                qualities.append(("original", 1, False))
            if args.matrix:
                qualities += [(quality + "-pixel", factor, True) for quality, factor, _ in list(qualities)]
            for quality, factor, pixel in qualities:
                if args.qualities and quality not in args.qualities:
                    continue
                key = f"{source.name}-{quality}"
                output = args.output / f"{key}.mpkg"
                def measure(destination, library=None):
                    child_env = dict(env)
                    if library:
                        child_env["DYLD_LIBRARY_PATH"] = str(library.resolve())
                    started = time.perf_counter()
                    child = subprocess.run(
                        [str(args.binary.resolve()), "--export", str(source), str(destination), str(factor), str(pixel).lower()],
                        env=child_env, capture_output=True, text=True, timeout=600, check=True)
                    duration = time.perf_counter() - started
                    hasher = hashlib.sha256()
                    with destination.open("rb") as stream:
                        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                            hasher.update(chunk)
                    return child, duration, hasher.hexdigest()
                previous = None
                if args.baseline_library:
                    old_output = args.output / f"{key}-baseline.mpkg"
                    # Alternate order so the second invocation's disk cache or
                    # thermal state does not always favor the same version.
                    if run % 2 == 0:
                        previous = measure(old_output, args.baseline_library)
                        result, elapsed, digest = measure(output, args.candidate_library)
                    else:
                        result, elapsed, digest = measure(output, args.candidate_library)
                        previous = measure(old_output, args.baseline_library)
                    assert digest == previous[2], f"Paired export bytes changed: {key}"
                else:
                    result, elapsed, digest = measure(output)
                tools = collections.defaultdict(lambda: dict(calls=0, worker_seconds=0.0))
                for line in result.stderr.splitlines():
                    if line.startswith("MIRAGE_EXPORT_TIMING "):
                        event = json.loads(line.removeprefix("MIRAGE_EXPORT_TIMING "))
                        tool = tools[event["tool"]]
                        tool["calls"] += 1
                        tool["worker_seconds"] += event["seconds"]
                if args.baseline:
                    assert key in baseline, f"Missing baseline case: {key}"
                    assert digest == baseline[key]["sha256"], f"Export bytes changed: {key}"
                if key in cases:
                    assert digest == cases[key]["sha256"], f"Non-deterministic export: {key}"
                case = cases.setdefault(key, dict(group=group.name, quality=quality, pixel_art=pixel,
                                                  reduction=factor, sha256=digest,
                                                  bytes=output.stat().st_size, runs=[]))
                row = dict(seconds=elapsed, tools=dict(tools))
                if previous:
                    row["baseline_seconds"] = previous[1]
                case["runs"].append(row)
                case["median_seconds"] = statistics.median(row["seconds"] for row in case["runs"])
                if previous:
                    case["baseline_median_seconds"] = statistics.median(row["baseline_seconds"] for row in case["runs"])
                report = dict(cases=cases, total_median_seconds=sum(c["median_seconds"] for c in cases.values()))
                (args.output / "benchmark.json").write_text(json.dumps(report, indent=2) + "\n")
                paired = f" (baseline {previous[1]:.3f}s)" if previous else ""
                print(f"{run + 1}/{args.runs} {key}: {elapsed:.3f}s{paired}", flush=True)
    if args.baseline:
        assert cases.keys() == baseline.keys(), "Baseline and current case sets differ"


if __name__ == "__main__":
    main()
