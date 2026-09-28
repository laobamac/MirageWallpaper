# Mobile scene export validation

Build the Debug app with code coverage disabled to compare timings, then compile
the export harness with the real FFmpeg and EtcTool executables:

```sh
xcodebuild -quiet -project 'Mirage/Mirage Wallpaper.xcodeproj' \
  -scheme 'Mirage Wallpaper' -configuration Debug -destination 'platform=macOS' \
  -derivedDataPath Mirage/build/DD CODE_SIGNING_ALLOWED=NO ENABLE_CODE_COVERAGE=NO build
python3 Mirage/Tests/run_mobile_export_options.py --derived-data Mirage/build/DD \
  --tools /path/to/SceneMobileTools --references /path/to/we
```

The runner prints the `Artifacts` folder containing `MirageMobileOptionsReview`.
The harness links the app's Debug dylib; rebuilding that app changes the code
executed by the harness. Finish baseline exports **before** rebuilding. Use
different output directories for the baseline and candidate, with identical
build settings and encoder executables. Do not build or run other benchmarks
while timing an export.

```sh
python3 Mirage/Tests/benchmark_mobile_export.py --binary /path/to/MirageMobileOptionsReview \
  --references /path/to/we --output Mirage/build/export-before --runs 3 --matrix
# Rebuild the candidate, then:
python3 Mirage/Tests/benchmark_mobile_export.py --binary /path/to/MirageMobileOptionsReview \
  --references /path/to/we --output Mirage/build/export-after --runs 3 --matrix \
  --baseline Mirage/build/export-before/benchmark.json
```

`--matrix` covers original, half and quarter resolution with pixel-art
optimization both off and on (48 exports for the eight sample wallpapers).
Each output must match the baseline SHA-256 and remain deterministic across
runs. Medians are measured per case. This is a Debug harness benchmark, not an
independent measurement of the packaged Release app or Android playback.

For interleaved timing, save each build's `Mirage Wallpaper.debug.dylib` in a
separate directory before rebuilding. `--baseline-library` and
`--candidate-library` select those directories through `DYLD_LIBRARY_PATH`.
The runner alternates execution order and verifies both versions' output hashes
for every pair. Both libraries must be compatible with the same harness.

```sh
python3 Mirage/Tests/benchmark_mobile_export.py --binary /path/to/MirageMobileOptionsReview \
  --references /path/to/we --output Mirage/build/export-paired --runs 3 --matrix \
  --groups 2 3 5 6 7 8 --qualities high high-pixel balanced-pixel original-pixel \
  --baseline-library /path/to/baseline-library --candidate-library /path/to/candidate-library
```

Validate the existing output against Wallpaper Engine's reference packages and
exercise the PNG decoder with independently constructed pixels:

```sh
python3 Mirage/Tests/validate_mobile_export_samples.py --references /path/to/we \
  --output Mirage/build/export-after --matrix --existing
PYTHONDONTWRITEBYTECODE=1 python3 Mirage/Tests/verify_mobile_export_pixels.py \
  --binary /path/to/MirageMobileOptionsReview --output Mirage/build/export-pixels \
  --expect-native
```

The pixel checks include hidden RGB under zero alpha, partial alpha, RGB padding,
row cropping, 16-bit, indexed, transparent-key and interlaced PNGs. The last four
formats must retain the FFmpeg fallback. `--expect-native` needs a Debug exporter
because it also checks decoder calls using the opt-in timing log.

The reference comparison checks archive entries, texture layout and metadata,
scene/project JSON (with float tolerance), and unchanged resources. Shader code
is not compared, and encoded ETC2 pixels may differ from WE's encoder.

In the supplied eight-group corpus, group 8's `高优化3232289987.mpkg` is byte-for-byte
identical to `高不优化3232289987.mpkg` (SHA-256
`1db8ac069803dcf924c515cd62c55cc3ed98ebe951bddc012484f344d8991af4`). This cannot
establish the enabled pixel-art rule independently. The validator reports this
comparison as a failure in `validation-failures.json`; it does not suppress it
or change export behavior to fit the duplicate reference. The 47 other reference
combinations and all 48 baseline/candidate hash comparisons can be checked.
