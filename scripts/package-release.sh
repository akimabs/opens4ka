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
MOUNT_ROOT="$(mktemp -d)"
VOLUME_NAME="OpenS4ka ${VERSION}"
RW_DMG="$RELEASE_DIR/.OpenS4ka-${VERSION}-macOS-${ARCH}.rw.dmg"
MOUNT_POINT=""

cleanup() {
    if [[ -n "$MOUNT_POINT" && -d "$MOUNT_POINT" ]]; then
        hdiutil detach "$MOUNT_POINT" -force >/dev/null 2>&1 || true
    fi
    rm -rf "$STAGE" "$MOUNT_ROOT" "$RW_DMG"
}
trap cleanup EXIT

mkdir -p "$RELEASE_DIR"
rm -rf "$RELEASE_DIR/OpenS4ka.app" "$ARCHIVE" "$CHECKSUM" "$DMG" "$DMG_CHECKSUM" "$RW_DMG"

OPENS4KA_OUTPUT_DIR="$RELEASE_DIR" \
OPENS4KA_PRESERVE_STATE=0 \
    "$SCRIPT_DIR/package-app.sh"

ditto -c -k --norsrc --keepParent "$RELEASE_DIR/OpenS4ka.app" "$ARCHIVE"
(
    cd "$RELEASE_DIR"
    shasum -a 256 "$(basename "$ARCHIVE")" > "$(basename "$CHECKSUM")"
)

ditto "$RELEASE_DIR/OpenS4ka.app" "$STAGE/OpenS4ka.app"
mkdir -p "$STAGE/.background"
ln -s /Applications "$STAGE/Applications"
xcrun swift "$SCRIPT_DIR/create-dmg-background.swift" \
    "$ROOT_DIR/Sources/OpenS4kaMac/Resources/opens4ka-logo.png" \
    "$STAGE/.background/dmg-background.png"
SetFile -a V "$STAGE/.background"

# Finder stores the window geometry, icon positions and background in .DS_Store.
# Configure those values on a writable image first, then ship a compressed copy.
hdiutil create \
    -volname "$VOLUME_NAME" \
    -srcfolder "$STAGE" \
    -fs HFS+ \
    -format UDRW \
    -ov "$RW_DMG"
hdiutil attach \
    -readwrite \
    -noverify \
    -noautoopen \
    -mountroot "$MOUNT_ROOT" \
    "$RW_DMG" >/dev/null
MOUNT_POINT="$MOUNT_ROOT/$VOLUME_NAME"
if [[ ! -d "$MOUNT_POINT" ]]; then
    echo "Unable to locate mounted DMG at: $MOUNT_POINT" >&2
    exit 1
fi

SetFile -a V "$MOUNT_POINT/.background"
osascript "$SCRIPT_DIR/configure-dmg.applescript" "$MOUNT_POINT"
sync
hdiutil detach "$MOUNT_POINT" >/dev/null
MOUNT_POINT=""

hdiutil convert "$RW_DMG" \
    -format UDZO \
    -imagekey zlib-level=9 \
    -ov \
    -o "$DMG" >/dev/null
(
    cd "$RELEASE_DIR"
    shasum -a 256 "$(basename "$DMG")" > "$(basename "$DMG_CHECKSUM")"
)

echo "Release archive: $ARCHIVE"
echo "Checksum: $CHECKSUM"
echo "Disk image: $DMG"
echo "Checksum: $DMG_CHECKSUM"
