#!/bin/zsh
# Builds a clean, distributable macOS archive without local saves, logs or shader caches.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

RELEASE_DIR="${OPENS4KA_RELEASE_DIR:-$ROOT_DIR/dist/release}"
VERSION="$(plutil -extract CFBundleShortVersionString raw -o - "$ROOT_DIR/Resources/Info.plist")"
ARCH="${OPENS4KA_ARCH:-$(uname -m)}"
ARCHIVE="$RELEASE_DIR/OpenS4ka-${VERSION}-macOS-${ARCH}.zip"
CHECKSUM="$ARCHIVE.sha256"
DMG="$RELEASE_DIR/OpenS4ka-${VERSION}-macOS-${ARCH}.dmg"
DMG_CHECKSUM="$DMG.sha256"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$RELEASE_DIR"
rm -rf "$RELEASE_DIR/OpenS4ka.app" "$ARCHIVE" "$CHECKSUM" "$DMG" "$DMG_CHECKSUM"

OPENS4KA_OUTPUT_DIR="$RELEASE_DIR" \
OPENS4KA_PRESERVE_STATE=0 \
    "$SCRIPT_DIR/package-app.sh"

ditto -c -k --norsrc --keepParent "$RELEASE_DIR/OpenS4ka.app" "$ARCHIVE"
(
    cd "$RELEASE_DIR"
    shasum -a 256 "$(basename "$ARCHIVE")" > "$(basename "$CHECKSUM")"
)

ditto "$RELEASE_DIR/OpenS4ka.app" "$STAGE/OpenS4ka.app"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "OpenS4ka ${VERSION}" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
(
    cd "$RELEASE_DIR"
    shasum -a 256 "$(basename "$DMG")" > "$(basename "$DMG_CHECKSUM")"
)

echo "Release archive: $ARCHIVE"
echo "Checksum: $CHECKSUM"
echo "Disk image: $DMG"
echo "Checksum: $DMG_CHECKSUM"
