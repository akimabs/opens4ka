# OpenS4ka contribution guide

## Project overview

OpenS4ka is a native macOS launcher and runtime for original Xbox games, statically recompiled to Apple Silicon arm64
C++ with [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp). It does not emulate the Xbox: each game's x86
code is converted to C++ ahead of time and built with a shared host. The launcher is SwiftUI and starts one native
runtime process per game. The host uses Vulkan through MoltenVK and SDL3 for windowing, input, and audio.

> [!IMPORTANT]
> OpenS4ka is an unofficial project for learning purposes and personal use. It is not affiliated with, endorsed by,
> or sponsored by Microsoft, Xbox, or the game copyright holders. Use only game files from your ISO.

| Game                            | Runtime           | Status   |
| ------------------------------- | ----------------- | -------- |
| Mortal Kombat: Shaolin Monks    | `OpenS4kaXbox`    | playable |
| Digimon Rumble Arena 2 (Europe) | `OpenS4kaDigimon` | playable |
| Need for Speed: Most Wanted     | `OpenS4kaNFSMW`   | playable |

The repository does not contain game images or game-derived binaries. Contributors use their own game images locally.

## Build and release

An agent building the project on an Apple Silicon Mac needs macOS 13 or later, Xcode command line tools with Swift 6,
CMake, Ninja, Python 3, and the Vulkan dependencies:

```sh
brew install cmake ninja molten-vk vulkan-headers vulkan-loader
```

If a task changes a game runtime, the contributor also needs their own copy of that game. Recompiling creates local
`assets/` and `generated/` directories, which are intentionally ignored by Git. `.xbe` files are only needed for this
local recompilation step; people downloading a release do not need them:

```sh
scripts/recompile.sh mk      "/path/to/Mortal Kombat Shaolin Monks.iso"
scripts/recompile.sh digimon "/path/to/Digimon Rumble Arena 2.iso"
scripts/recompile.sh nfs-mw  "/path/to/Need for Speed Most Wanted.iso"
```

Build and launch the app locally with:

```sh
scripts/package-app.sh
OPENS4KA_GAMES="mk digimon" scripts/package-app.sh
open dist/OpenS4ka.app
```

Create clean release assets with:

```sh
scripts/package-release.sh
```

This writes a DMG, ZIP, and SHA-256 checksum for each in `dist/release/`. The release DMG contains
`OpenS4ka.app` and an Applications shortcut. Keep `dist/` out of Git.

## Project layout

```text
Sources/            SwiftUI launcher (OpenS4kaMac) and the process bridge (OpenS4kaRuntime)
contribute.md       project guide and AI-agent contribution prompt
demo/               public demo images, initial screen, and app logo assets
runtime/            the game runtime: CMake, macOS entry point
  host/             Vulkan renderer, input, audio, texture patches
  game/             Direct3D 8, DirectSound, settings; games/<game>/ for each game's own code
sdk/                XboxXBE-Recomp runtime and recompiler (vendored)
config/<game>.toml  recompiler configuration per game
tools/              recompiler build, xdvdfs.py (reads files from an Xbox disc image)
scripts/            recompile.sh, package-app.sh, package-release.sh
Resources/          bundle plists, MoltenVK ICD, textures/<game>/*.patch
```

# AI contribution prompt

Copy the prompt below into your AI coding agent. Replace the text in the `TASK` section with the change you want.

```text
You are the AI coding agent contributing to OpenS4ka for macOS. Work directly in the repository and complete the task
end to end. Inspect the existing code before making assumptions, make the smallest coherent change, and explain the
result and validation when you finish.

## TASK

Describe the feature, bug fix, documentation change, or investigation here.

## Project context

- OpenS4ka is a native Apple Silicon macOS 13+ launcher and runtime for original Xbox games.
- This is an unofficial project for learning purposes and personal use; it is not affiliated with or endorsed by the
  platform or game copyright holders.
- The launcher is SwiftUI/Swift. It starts one native runtime process per game.
- The runtime is C++ built with CMake/Ninja, using Vulkan through MoltenVK and SDL3 for windowing, input, and audio.
- Recompiled game code comes from XboxXBE-Recomp. This is a recompiled runtime, not an emulator.
- Current runtimes are Mortal Kombat: Shaolin Monks (`OpenS4kaXbox`), Digimon Rumble Arena 2 Europe
  (`OpenS4kaDigimon`), and Need for Speed: Most Wanted (`OpenS4kaNFSMW`).
- The repository does not contain game images or game-derived binaries. Contributors use their own game images locally.
- A release is distributed as a macOS DMG containing `OpenS4ka.app`; users do not download or run the inner Mach-O
  executable directly.

## Repository map

- `Sources/`: SwiftUI launcher and the Swift process bridge.
- `runtime/`: C++ runtime, CMake files, macOS entry point, renderer, input, audio, and game-specific code.
- `sdk/`: vendored XboxXBE-Recomp runtime and recompiler support.
- `config/`: recompiler configuration for each game.
- `tools/`: recompiler tools and Xbox disc-image readers.
- `scripts/`: game recompile, app packaging, release packaging, and asset helpers.
- `Resources/`: app metadata, icon, MoltenVK configuration, and texture patches.
- `demo/`: public screenshots, initial screen artwork, and app logo assets for documentation or showcases.
- `contribute.md`: this AI-agent contribution template.

## Rules

1. Read `README.md`, relevant source files, and any local agent instructions before editing.
2. Inspect the current Git status first. Preserve unrelated user changes already present in the worktree.
3. Do not reset, clean, overwrite, or revert user work without explicit permission.
4. Never add or commit ISO files, `.xbe` files, `assets/`, `generated/`, `dist/`, runtime build directories, local saves,
   logs, shader caches, or other generated game data.
5. Do not introduce copyrighted game files or require users to obtain files from an unauthorized source.
6. Keep the macOS app bundle structure and `Resources/Info.plist`/`AppIcon.icns` metadata valid when packaging changes.
7. Prefer an implementation that fits the existing architecture. Avoid broad rewrites and unrelated formatting changes.
8. Do not commit, push, publish a release, or send messages unless the user explicitly asks for that action.

## Workflow

1. Inspect the relevant implementation and identify the smallest set of files that must change.
2. State a short plan, then implement it directly.
3. Add or update documentation when the user-facing workflow changes.
4. Run focused validation appropriate to the changed area:
   - Swift or launcher changes: `swift test`
   - Shell changes: `zsh -n` on the changed scripts
   - Runtime changes: use the existing CMake/Ninja build or packaging scripts when available
   - Any change: `git diff --check`
5. If a check cannot run, explain why and run the closest useful alternative.
6. Report changed files, commands run, test results, and remaining limitations.

## Local dependencies

An agent building the project on an Apple Silicon Mac needs macOS 13 or later, Xcode command line tools with Swift 6,
CMake, Ninja, Python 3, and:

    brew install cmake ninja molten-vk vulkan-headers vulkan-loader

If a task changes a game runtime, the contributor also needs their own copy of that game. Recompiling creates local
`assets/` and `generated/` directories, which are intentionally ignored by Git. `.xbe` files are only needed for this
local recompilation step; people downloading a release do not need them.

    scripts/recompile.sh mk      "/path/to/Mortal Kombat Shaolin Monks.iso"
    scripts/recompile.sh digimon "/path/to/Digimon Rumble Arena 2.iso"
    scripts/recompile.sh nfs-mw  "/path/to/Need for Speed Most Wanted.iso"

For a local app build:

    scripts/package-app.sh
    open dist/OpenS4ka.app

For a clean release containing a DMG, ZIP, and SHA-256 checksums:

    scripts/package-release.sh

Start by checking the actual repository state. Do not assume that this prompt is newer than the files in the checkout.
```

For a useful request, make the `TASK` section concrete. For example:

```text
Fix the game library scanner so an unavailable game folder is not recreated or mounted. Add a focused regression test,
preserve the existing folder behavior, and run the relevant Swift tests.
```
