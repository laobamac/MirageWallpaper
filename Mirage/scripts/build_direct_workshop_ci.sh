#!/bin/bash
#
#  Mirage Wallpaper
#
#  Copyright © 2026 王孝慈. All rights reserved.
#

set -euo pipefail

PRIVATE_REPOSITORY="${MIRAGE_DIRECT_WORKSHOP_REPOSITORY:-}"
PRIVATE_REF="${MIRAGE_DIRECT_WORKSHOP_REF:-}"

if [[ -z "$PRIVATE_REPOSITORY" && -z "$PRIVATE_REF" && -z "${MIRAGE_DIRECT_WORKSHOP_DEPLOY_KEY:-}" ]]; then
    echo "[direct-workshop] Private component is not configured; building the standard edition."
    exit 0
fi

if [[ ! "$PRIVATE_REPOSITORY" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ||
      ! "$PRIVATE_REF" =~ ^[A-Fa-f0-9]{40}$ || -z "${MIRAGE_DIRECT_WORKSHOP_DEPLOY_KEY:-}" ]]; then
    echo "::error::Configure MIRAGE_DIRECT_WORKSHOP_REPOSITORY, MIRAGE_DIRECT_WORKSHOP_REF (full commit SHA), and MIRAGE_DIRECT_WORKSHOP_DEPLOY_KEY together."
    exit 1
fi

SOURCE_ROOT="${GITHUB_WORKSPACE:?GITHUB_WORKSPACE is required}"
WORK="$(mktemp -d "${RUNNER_TEMP:?RUNNER_TEMP is required}/mirage-direct-workshop.XXXXXX")"
BUNDLE="$RUNNER_TEMP/mirage-direct-workshop-bundle"
trap 'rm -rf "$WORK"' EXIT
umask 077
printf '%s\n' "$MIRAGE_DIRECT_WORKSHOP_DEPLOY_KEY" > "$WORK/deploy-key"
unset MIRAGE_DIRECT_WORKSHOP_DEPLOY_KEY
printf '%s\n' '[ssh.github.com]:443 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl' > "$WORK/known-hosts"
GIT_SSH_COMMAND="$(python3 - "$WORK" <<'PY'
from pathlib import Path
import shlex
import sys

root = Path(sys.argv[1])
print(shlex.join(["ssh", "-F", "/dev/null", "-i", str(root / "deploy-key"), "-o", "IdentitiesOnly=yes",
                  "-o", "StrictHostKeyChecking=yes", "-o", "BatchMode=yes", "-o", "ConnectTimeout=20",
                  "-o", "UserKnownHostsFile=" + str(root / "known-hosts")]))
PY
)"
export GIT_SSH_COMMAND
git init --quiet "$WORK/source"
if ! git -C "$WORK/source" fetch --quiet --depth=1 "ssh://git@ssh.github.com:443/$PRIVATE_REPOSITORY.git" "$PRIVATE_REF" > "$WORK/fetch.log" 2>&1; then
    echo "::error::Could not fetch the pinned private component. Check the repository, commit SHA, and read-only deploy key."
    exit 1
fi
git -C "$WORK/source" checkout --quiet --detach FETCH_HEAD
rm -f "$WORK/deploy-key"
unset GIT_SSH_COMMAND

if [[ ! -f "$WORK/source/build.sh" || ! -f "$WORK/source/Service/Service.csproj" ||
      ! -f "$WORK/source/Service/VerificationKey.cs" || ! -f "$WORK/source/Service/ContentKey.cs" ]]; then
    echo "::error::The private repository does not contain the required component sources."
    exit 1
fi
if find "$WORK/source" \( -name secrets -o -name '*.pem' -o -name '*.p12' \) -print -quit | grep -q .; then
    echo "::error::The private repository contains signing material. Remove it from the private repository before building."
    exit 1
fi

if ! CI=true MIRAGE_SOURCE_ROOT="$SOURCE_ROOT" bash "$WORK/source/build.sh" > "$WORK/build.log" 2>&1; then
    echo "::error::Private component build or tests failed. Reproduce with build.sh at the pinned private commit; private compiler output is not published."
    exit 1
fi

python3 - "$WORK/source/dist" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
required = [
    "MirageDirectWorkshop.dll", "MirageDirectWorkshop.deps.json",
    "MirageDirectWorkshop.runtimeconfig.json", "SteamKit2.dll",
    "Licenses/MirageDirectWorkshop-LICENSE.txt", "Licenses/LGPL-2.1.txt",
    "Licenses/SteamKit2-NOTICE.txt",
]
if any(not (root / name).is_file() for name in required):
    sys.exit("::error::The private component output is incomplete.")
for path in root.rglob("*"):
    if path.is_symlink():
        sys.exit("::error::Symlinks are not permitted in the private component bundle.")
    if not path.is_file():
        continue
    if path.suffix.lower() not in {".dll", ".json", ".txt"} or "private" in path.name.lower():
        sys.exit("::error::Unexpected file in the private component bundle.")
    if b"PRIVATE KEY-----" in path.read_bytes():
        sys.exit("::error::Signing material is not permitted in the private component bundle.")
PY

rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"
cp -R "$WORK/source/dist/." "$BUNDLE/"
find "$BUNDLE" -type d -exec chmod 755 {} +
find "$BUNDLE" -type f -exec chmod 644 {} +
printf 'MIRAGE_DIRECT_WORKSHOP_BUNDLE=%s\n' "$BUNDLE" >> "${GITHUB_ENV:?GITHUB_ENV is required}"
echo "[direct-workshop] Private component tests and build passed; binary bundle ready."
