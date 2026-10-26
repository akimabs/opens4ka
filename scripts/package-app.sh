#!/bin/zsh
# Builds every game's runtime (runtime/build/<game>) and the launcher, then bundles them into dist/OpenS4ka.app.
# OPENS4KA_GAMES="mk digimon" limits the runtimes built; the others keep what the bundle has.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"
OUTPUT_DIR="${OPENS4KA_OUTPUT_DIR:-$ROOT_DIR/dist}"
PRESERVE_STATE="${OPENS4KA_PRESERVE_STATE:-1}"

case "$PRESERVE_STATE" in
    0|1) ;;
    *) echo "OPENS4KA_PRESERVE_STATE must be 0 or 1" >&2; exit 1 ;;
esac

# game : runtime executable : Info.plist
RUNTIMES=(
    "mk:OpenS4kaXbox:RuntimeInfo.plist"
    "digimon:OpenS4kaDigimon:DigimonRuntimeInfo.plist"
    "nfs-mw:OpenS4kaNFSMW:NFSMWRuntimeInfo.plist"
)
GAMES=(${=OPENS4KA_GAMES:-mk digimon nfs-mw})

for entry in $RUNTIMES; do
    game="${entry%%:*}"; rest="${entry#*:}"; target="${rest%%:*}"
    (( ${GAMES[(Ie)$game]} )) || continue
    cmake -S "$ROOT_DIR/runtime" -B "$ROOT_DIR/runtime/build/$game" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DOPENS4KA_GAME="$game"
    cmake --build "$ROOT_DIR/runtime/build/$game" --target "$target" --parallel 4
done

xcrun -sdk macosx metal -std=metal3.0 "$ROOT_DIR/Shaders/CRT.metal" \
    -o "$ROOT_DIR/Sources/OpenS4kaMac/Resources/CRT.metallib"
swift build -c release
BIN_DIR="$(swift build -c release --show-bin-path)"
APP="$OUTPUT_DIR/OpenS4ka.app"
VULKAN_LOADER="/opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib"
MOLTENVK="/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib"

if [[ ! -f "$VULKAN_LOADER" || ! -f "$MOLTENVK" ]]; then
    echo "Homebrew Vulkan loader/MoltenVK is required to package the macOS runtime." >&2
    exit 1
fi

# runtimes keep saves (hdd), logs and shader caches beside their binary: carry them over the rebuild,
# and a runtime not built this time keeps its binary too
KEEP=""
trap '[[ -z "$KEEP" ]] || rm -rf "$KEEP"' EXIT
if [[ "$PRESERVE_STATE" == 1 ]]; then
    KEEP="$(mktemp -d)"
    for entry in $RUNTIMES; do
        rest="${entry#*:}"; target="${rest%%:*}"
        dir="$APP/Contents/Resources/Runtime/$target.app/Contents/MacOS"
        if [[ -d "$dir" ]]; then
            mkdir -p "$KEEP/$target"
            rsync -a "$dir/" "$KEEP/$target/"
        fi
    done
fi

mkdir -p "$OUTPUT_DIR"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources/Runtime" \
    "$APP/Contents/Resources/vulkan/icd.d"
cp "$BIN_DIR/OpenS4ka" "$APP/Contents/MacOS/OpenS4ka"
ARTWORK_BUNDLE="$APP/Contents/Resources/OpenS4kaMac_OpenS4kaMac.bundle"
mkdir -p "$ARTWORK_BUNDLE"
for art in mortal-kombat-ico.webp digimon-rumble-arena-2.webp nfs-most-wanted.webp opens4ka-bg.webp opens4ka-logo.png \
    CRT.metallib; do
    cp "$ROOT_DIR/Sources/OpenS4kaMac/Resources/$art" "$ARTWORK_BUNDLE/"
done
cp "$VULKAN_LOADER" "$APP/Contents/Frameworks/libvulkan.1.dylib"
cp "$MOLTENVK" "$APP/Contents/Frameworks/libMoltenVK.dylib"
cp "$ROOT_DIR/Resources/MoltenVK_icd.json" "$APP/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json"
cp "$ROOT_DIR/Resources/Info.plist" "$APP/Contents/Info.plist"
cp "$ROOT_DIR/Resources/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"

for entry in $RUNTIMES; do
    game="${entry%%:*}"; rest="${entry#*:}"; target="${rest%%:*}"; plist="${rest#*:}"
    contents="$APP/Contents/Resources/Runtime/$target.app/Contents"
    mkdir -p "$contents/MacOS"
    [[ -z "$KEEP" || ! -d "$KEEP/$target" ]] || rsync -a "$KEEP/$target/" "$contents/MacOS/"
    cp "$ROOT_DIR/Resources/$plist" "$contents/Info.plist"
    built="$ROOT_DIR/runtime/build/$game/bin/$target"
    if (( ${GAMES[(Ie)$game]} )); then
        cp "$built" "$contents/MacOS/$target"
        install_name_tool -change "$VULKAN_LOADER" "@rpath/libvulkan.1.dylib" "$contents/MacOS/$target"
        install_name_tool -add_rpath "@loader_path/../../../../../Frameworks" "$contents/MacOS/$target"
    fi
    # the PlayStation prompts (scripts/ps_prompts.py), replaced whole so an older set never stays
    rm -rf "$contents/MacOS/textures"
    if [[ -d "$ROOT_DIR/Resources/textures/$game" ]]; then
        mkdir -p "$contents/MacOS/textures"
        cp "$ROOT_DIR/Resources/textures/$game/"*.patch "$contents/MacOS/textures/"
    fi
done

echo "Packaged: $APP"
