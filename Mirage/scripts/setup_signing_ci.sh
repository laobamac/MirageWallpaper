#!/bin/bash
# Copyright © 2026 王孝慈. All rights reserved.
set -euo pipefail
for variable in APPLE_DEVELOPER_ID_APPLICATION_P12 APPLE_DEVELOPER_ID_APPLICATION_P12_PASSWORD APPLE_NOTARY_APPLE_ID APPLE_NOTARY_PASSWORD APPLE_DEVELOPER_TEAM_ID; do
  [[ -n "${!variable}" ]] || {
    echo "::error::Repository secret $variable is missing."
    exit 1
  }
done
certificate_path="$RUNNER_TEMP/developer-id-application.p12"
keychain_path="$RUNNER_TEMP/mirage-signing.keychain-db"
keychain_password="$(openssl rand -base64 32)"
trap 'rm -f "$certificate_path"' EXIT
printf '%s' "$APPLE_DEVELOPER_ID_APPLICATION_P12" | base64 --decode > "$certificate_path"
security create-keychain -p "$keychain_password" "$keychain_path"
security set-keychain-settings -lut 21600 "$keychain_path"
security unlock-keychain -p "$keychain_password" "$keychain_path"
security import "$certificate_path" -P "$APPLE_DEVELOPER_ID_APPLICATION_P12_PASSWORD" -T /usr/bin/codesign -t cert -f pkcs12 -k "$keychain_path"
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$keychain_password" "$keychain_path"
security list-keychains -d user -s "$keychain_path"
identity="$(security find-identity -v -p codesigning "$keychain_path" | sed -n 's/.*"\(Developer ID Application:.*\)"/\1/p')"
[[ -n "$identity" && "$identity" != *$'\n'* ]] || {
  echo "::error::The signing certificate secret must contain exactly one Developer ID Application identity."
  exit 1
}
[[ "$identity" == "Developer ID Application: "*" ($APPLE_DEVELOPER_TEAM_ID)" ]] || {
  echo "::error::The Developer ID Application certificate does not match APPLE_DEVELOPER_TEAM_ID."
  exit 1
}
xcrun notarytool store-credentials mirage-notary \
  --keychain "$keychain_path" \
  --apple-id "$APPLE_NOTARY_APPLE_ID" \
  --password "$APPLE_NOTARY_PASSWORD" \
  --team-id "$APPLE_DEVELOPER_TEAM_ID"
{
  echo "MIRAGE_SIGN_IDENTITY=$identity"
  echo "MIRAGE_DEVELOPMENT_TEAM=$APPLE_DEVELOPER_TEAM_ID"
  echo "MIRAGE_NOTARY_PROFILE=mirage-notary"
  echo "MIRAGE_NOTARY_KEYCHAIN=$keychain_path"
} >> "$GITHUB_ENV"
