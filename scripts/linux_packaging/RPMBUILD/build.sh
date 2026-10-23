#!/usr/bin/env bash
# Build the RPM from a clean git archive so %prep sees the same source tree in
# local builds and CI. Generated rpmbuild trees stay under this directory and
# are never mixed with repository source files.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOP_DIR="$ROOT_DIR/RPMBUILD"
SPEC="$TOP_DIR/SPECS/miragewallpaper.spec"
VERSION="${VERSION:-1.0.7}"
SOURCE_DIR="$TOP_DIR/SOURCES"
ARCHIVE="$SOURCE_DIR/miragewallpaper-$VERSION.tar.gz"

mkdir -p "$SOURCE_DIR" "$TOP_DIR/BUILD" "$TOP_DIR/BUILDROOT" \
    "$TOP_DIR/RPMS" "$TOP_DIR/SRPMS"
git -C "$ROOT_DIR" archive --format=tar.gz \
    --prefix="MirageWallpaper-$VERSION/" HEAD > "$ARCHIVE"

rpmbuild -ba \
    --define "_topdir $TOP_DIR" \
    --define "mirage_version $VERSION" \
    "$SPEC"
