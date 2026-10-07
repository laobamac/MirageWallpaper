__copyright__ = "Copyright © 2026 王孝慈. All rights reserved."

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

sys.dont_write_bytecode = True
PINS = {
    "production": ("v1.4.2", "6864db532f1dbbdb621a8d0ec13f24edae318fd9269dd3dd0cdff791334bb1cb"),
    "diagnostics": ("v1.4.1", "9985f141902a17de818e264d17c1ce334b748e499ee02fcb4703e4dc0038f89c"),
}
PATCH_ID = "color-attachment-transfer-source-v1"
ORIGINAL = "mtlTexDesc.allowGPUOptimizedContents = !_image->_is2DViewOn3DImageCompatible && !_image->_isBlockTexelViewCompatible;"
PATCHED = "mtlTexDesc.allowGPUOptimizedContents = !_image->_is2DViewOn3DImageCompatible && !_image->_isBlockTexelViewCompatible && !mvkAreAllFlagsEnabled(_image->_usage, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);"
PATCH_SHA256 = hashlib.sha256((ORIGINAL + "\n" + PATCHED).encode()).hexdigest()
DEPENDENCIES = [
    ("cereal", "USCiLab/cereal", "cereal"),
    ("Vulkan-Headers", "KhronosGroup/Vulkan-Headers", "Vulkan-Headers"),
    ("SPIRV-Cross", "KhronosGroup/SPIRV-Cross", "SPIRV-Cross"),
    ("SPIRV-Tools", "KhronosGroup/SPIRV-Tools", "SPIRV-Tools"),
    ("SPIRV-Headers", "KhronosGroup/SPIRV-Headers", "SPIRV-Tools/external/spirv-headers"),
    ("Vulkan-Tools", "KhronosGroup/Vulkan-Tools", "Vulkan-Tools"),
    ("Volk", "zeux/volk", "Volk"),
]


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def extract_archive(data, destination):
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        total = 0
        links = []
        for member in archive.getmembers():
            parts = Path(member.name).parts[1:]
            if not parts:
                continue
            if member.islnk() or ".." in parts or Path(*parts).is_absolute():
                raise ValueError("Unsafe archive entry")
            target = destination.joinpath(*parts)
            if not target.resolve().is_relative_to(destination.resolve()):
                raise ValueError("Archive path escapes its destination")
            if member.issym():
                if Path(member.linkname).is_absolute() or not (target.parent / member.linkname).resolve().is_relative_to(destination.resolve()):
                    raise ValueError("Unsafe archive symlink")
                links.append((target, member.linkname))
            elif member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                total += member.size
                if total > 1024 * 1024 * 1024:
                    raise ValueError("Expanded archive exceeds size limit")
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output)
                target.chmod(member.mode & 0o755)
            else:
                raise ValueError("Unsupported archive entry")
        for target, link in links:
            if not (target.parent / link).resolve().is_relative_to(destination.resolve()):
                raise ValueError("Archive symlink chain escapes its destination")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.symlink_to(link)


def fetch_archive(url, destination, expected_sha256=None):
    with tempfile.TemporaryFile() as download:
        subprocess.run(["/usr/bin/curl", "--fail", "--location", "--retry", "3", "--max-time", "180",
                        "--max-filesize", str(128 * 1024 * 1024), "--silent", "--show-error", url],
                       check=True, stdout=download)
        download.seek(0)
        data = download.read(128 * 1024 * 1024 + 1)
    if len(data) > 128 * 1024 * 1024:
        raise ValueError("Archive exceeds size limit")
    digest = hashlib.sha256(data).hexdigest()
    if expected_sha256 is not None and digest != expected_sha256:
        raise ValueError("MoltenVK source archive checksum mismatch")
    extract_archive(data, destination)
    return digest


def patch_source(text):
    if text.count(ORIGINAL) != 1:
        raise ValueError("MoltenVK source does not match the expected unpatched version")
    return text.replace(ORIGINAL, PATCHED)


def run(command, cwd):
    print("Running:", " ".join(map(str, command)), flush=True)
    subprocess.run(command, cwd=cwd, check=True, timeout=5400)


def library_uuids(path):
    output = subprocess.check_output(["xcrun", "dwarfdump", "--uuid", str(path)], text=True)
    result = {arch: uuid.upper() for uuid, arch in re.findall(r"UUID: ([0-9A-Fa-f-]+) \(([^)]+)\)", output)}
    if not result or not set(result).issubset({"arm64", "x86_64"}):
        raise ValueError(f"Unexpected Mach-O architecture: {path}")
    return result


def validate_manifest(manifest, mode):
    version, checksum = PINS[mode]
    if (manifest.get("schema") != 2 or manifest.get("mode") != mode
            or manifest.get("version") != version or manifest.get("source_archive_sha256") != checksum
            or manifest.get("patch_id") != PATCH_ID or manifest.get("patch_sha256") != PATCH_SHA256):
        raise ValueError("MoltenVK manifest does not match the pinned build")
    expected = {"patched"} if mode == "production" else {"baseline", "patched"}
    if set(manifest.get("libraries", {})) != expected:
        raise ValueError("Incomplete MoltenVK library manifest")
    for variant in expected:
        entry = manifest["libraries"][variant]
        if not re.fullmatch(r"[a-f0-9]{64}", entry.get("sha256_unsigned", "")):
            raise ValueError("Missing MoltenVK library checksum")
        if set(entry.get("uuids", {})) != {"arm64", "x86_64"}:
            raise ValueError("MoltenVK build must contain both macOS architectures")


def verify_build(directory, mode):
    manifest = json.loads((directory / "manifest.json").read_text())
    validate_manifest(manifest, mode)
    for variant, entry in manifest["libraries"].items():
        path = directory / "libMoltenVK.dylib" if mode == "production" else directory / variant / "libMoltenVK.dylib"
        if file_sha256(path) != entry["sha256_unsigned"] or library_uuids(path) != entry["uuids"]:
            raise ValueError(f"MoltenVK build fingerprint mismatch: {path}")
    return manifest


def verify_bundle(app, build=None):
    app = app.resolve()
    metadata = app / "Contents/Resources/MoltenVK/manifest.json"
    manifest = verify_build(build, "production") if build else json.loads(metadata.read_text())
    validate_manifest(manifest, "production")
    info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
    required_arches = set(library_uuids(app / "Contents/MacOS" / info["CFBundleExecutable"]))
    expected_uuids = manifest["libraries"]["patched"]["uuids"]
    shared = app / "Contents/Extensions/MirageWallpaperExtension.appex/Contents"
    paths = {(shared / "Frameworks/libMoltenVK.dylib").resolve()}
    if (app / "Contents/Frameworks/libMoltenVK.dylib").resolve() not in paths:
        raise ValueError("App does not resolve the shared extension runtime")
    for component in ("Contents/Resources/Screen Savers/MirageScreenSaver.saver",
                      "Contents/Resources/Screen Savers/MirageDynamicLockScreen.saver"):
        for duplicated in ("Contents/Frameworks", "Contents/Resources/assets", "Contents/Resources/vulkan"):
            if (app / component / duplicated).exists():
                raise ValueError(f"Scene host duplicates the shared app payload: {component}/{duplicated}")
    diagnostic_root = app / "Contents/Resources/SceneDiagnostics"
    discovered = {path.resolve() for path in app.rglob("libMoltenVK.dylib") if not path.is_relative_to(diagnostic_root)}
    if discovered != paths:
        raise ValueError("Unexpected or missing production MoltenVK library locations")
    icds = [app / "Contents/Resources/Renderers/vulkan/icd.d/MoltenVK_icd.json",
            shared / "Resources/vulkan/icd.d/MoltenVK_icd.json"]
    for icd in icds:
        config = json.loads(icd.read_text())
        library = Path(config["ICD"]["library_path"])
        if library.is_absolute() or (icd.parent / library).resolve() not in paths:
            raise ValueError(f"ICD does not use a bundled production library: {icd}")
    records = {}
    for path in paths:
        if not path.is_relative_to(app):
            raise ValueError("Production library escapes its app bundle")
        uuids = library_uuids(path)
        if not required_arches.issubset(uuids) or any(expected_uuids.get(arch) != value for arch, value in uuids.items()):
            raise ValueError(f"An unpatched or incompatible MoltenVK library was bundled: {path}")
        records[str(path.relative_to(app))] = {"sha256": file_sha256(path), "uuids": uuids}
    if build:
        metadata.parent.mkdir(parents=True, exist_ok=True)
        manifest["bundled_libraries"] = records
        metadata.write_text(json.dumps(manifest, indent=2) + "\n")
        shutil.copytree(build / "Licenses", metadata.parent / "Licenses", dirs_exist_ok=True)
    elif manifest.get("bundled_libraries") != records:
        raise ValueError("Bundled MoltenVK checksums changed after packaging")
    print(f"Verified {len(records)} protected production libraries: {app}", flush=True)


def prepare_source(source, mode):
    version, checksum = PINS[mode]
    url = f"https://codeload.github.com/KhronosGroup/MoltenVK/tar.gz/refs/tags/{version}"
    source.mkdir(parents=True)
    fetch_archive(url, source, checksum)
    revisions = {}
    for name, repository, relative in DEPENDENCIES:
        revision = (source / "ExternalRevisions" / f"{name}_repo_revision").read_text().strip()
        if not re.fullmatch(r"[a-f0-9]{40}", revision):
            raise ValueError(f"Invalid revision for {name}")
        destination = source / "External" / relative
        destination.mkdir(parents=True, exist_ok=True)
        digest = fetch_archive(f"https://codeload.github.com/{repository}/tar.gz/{revision}", destination)
        revisions[name] = {"revision": revision, "archive_sha256": digest}
    with zipfile.ZipFile(source / "Templates/spirv-tools/build.zip") as archive:
        for name in archive.namelist():
            if Path(name).is_absolute() or ".." in Path(name).parts:
                raise ValueError("Unsafe pre-generated header path")
        archive.extractall(source / "External/SPIRV-Tools")
    env = os.environ.copy()
    env.update(SKIP_PACKAGING="Y")
    subprocess.run([
        "xcodebuild", "build", "-project", "ExternalDependencies.xcodeproj",
        "-scheme", "ExternalDependencies-macOS", "-configuration", "Release",
        "-destination", "generic/platform=macOS", "-derivedDataPath", "External/build/Intermediates/macOS",
        "CODE_SIGNING_ALLOWED=NO", "-quiet",
    ], cwd=source, env=env, check=True, timeout=5400)
    run(["bash", "-c", 'export PROJECT_DIR=. CONFIGURATION=Release; source Scripts/create_ext_lib_xcframeworks.sh; source Scripts/package_ext_libs_finish.sh'], source)
    return {"copyright": __copyright__, "schema": 2, "mode": mode, "version": version, "source": url,
            "source_archive_sha256": checksum, "dependencies": revisions, "patch_id": PATCH_ID,
            "patch_sha256": PATCH_SHA256, "upstream_fix": "https://github.com/KhronosGroup/MoltenVK/pull/2724",
            "toolchain": subprocess.check_output(["xcodebuild", "-version"], text=True).strip(), "libraries": {}}


def build_libraries(output, work, mode):
    if output.exists():
        verify_build(output, mode)
        print(f"Using verified MoltenVK build: {output}", flush=True)
        return
    source = work / "MoltenVK"
    if source.exists():
        raise ValueError("Source directory already exists")
    print(f"Build directory: {work}", flush=True)
    manifest = prepare_source(source, mode)
    image = source / "MoltenVK/MoltenVK/GPUObjects/MVKImage.mm"
    original = image.read_text()
    patched = patch_source(original)
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=output.name + ".", dir=output.parent))
    try:
        variants = [("patched", patched)] if mode == "production" else [("baseline", original), ("patched", patched)]
        for variant, text in variants:
            image.write_text(text)
            run(["xcodebuild", "build", "-project", "MoltenVKPackaging.xcodeproj",
                 "-scheme", "MoltenVK Package (macOS only)", "-configuration", "Release",
                 "-destination", "generic/platform=macOS", "CODE_SIGNING_ALLOWED=NO", "-quiet"], source)
            built = source / "Package/Latest/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib"
            target = staging / "libMoltenVK.dylib" if mode == "production" else staging / variant / "libMoltenVK.dylib"
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(built, target)
            run(["install_name_tool", "-id", "@rpath/libMoltenVK.dylib", str(target)], source)
            manifest["libraries"][variant] = {"sha256_unsigned": file_sha256(target), "uuids": library_uuids(target),
                                              "image_source_sha256": hashlib.sha256(text.encode()).hexdigest()}
        licenses = staging / "Licenses"
        licenses.mkdir()
        for directory in [source] + [source / "External" / item[2] for item in DEPENDENCIES]:
            for path in list(directory.glob("LICENSE*")) + list(directory.glob("NOTICE*")):
                if path.is_file():
                    shutil.copy2(path, licenses / str(path.relative_to(source)).replace("/", "_"))
        (staging / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        verify_build(staging, mode)
        staging.rename(output)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    print(f"Protected MoltenVK build: {output}", flush=True)


def main(default_mode="production"):
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=PINS, default=default_mode)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--work", type=Path)
    parser.add_argument("--verify", type=Path)
    parser.add_argument("--verify-bundle", type=Path)
    parser.add_argument("--record-bundle", type=Path)
    args = parser.parse_args()
    if args.verify:
        verify_build(args.verify.resolve(), args.mode)
    elif args.verify_bundle:
        verify_bundle(args.verify_bundle)
    elif args.record_bundle:
        if args.output is None:
            parser.error("--record-bundle requires --output pointing to the protected build")
        verify_bundle(args.record_bundle, args.output.resolve())
    elif args.output:
        work = args.work.resolve() if args.work else Path(tempfile.mkdtemp(prefix="mirage-moltenvk-"))
        build_libraries(args.output.resolve(), work, args.mode)
    else:
        parser.error("Specify --output, --verify, or --verify-bundle")


if __name__ == "__main__":
    main()
