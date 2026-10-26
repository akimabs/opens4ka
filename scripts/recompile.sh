#!/bin/zsh
# Turns a game's executable into C++: scripts/recompile.sh <mk|digimon|nfs-mw> [game.iso]
# With an ISO, its default.xbe is taken out to assets/<game>.xbe first. Writes generated/<game>, which
# runtime/build/<game> (scripts/package-app.sh) compiles.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
game="${1:?usage: scripts/recompile.sh <mk|digimon|nfs-mw> [game.iso]}"
[[ -f "config/$game.toml" ]] || { echo "no config/$game.toml" >&2; exit 1; }

mkdir -p assets
if [[ $# -ge 2 ]]; then
    python3 -I tools/xdvdfs.py --force "$2" default.xbe "assets/$game.xbe"
fi
[[ -f "assets/$game.xbe" ]] || { echo "no assets/$game.xbe: pass the game's ISO" >&2; exit 1; }

cmake -S tools -B tools/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build tools/build --target xbe_recomp
tools/build/xbe_recomp "config/$game.toml" "assets/$game.xbe" "generated/$game" --xdk sdk/xdk
echo "Recompiled: generated/$game"
