import AppKit
import Combine
import Foundation

@MainActor
final class GameLibrary: ObservableObject {
    struct Game: Identifiable {
        let id: String
        let title: String
        let subtitle: String
        let coverResource: String
        let folderNames: [String]
        let nameTokens: [String]
        let runtimeName: String
        let nativeRuntimeAvailable: Bool
    }

    let mortalKombat = Game(
        id: "mortal-kombat-xbox",
        title: "Mortal Kombat",
        subtitle: "Shaolin Monks",
        coverResource: "mortal-kombat-ico",
        folderNames: ["Mortal Kombat", "Shaolin Monks", "ShaolinMonks"],
        nameTokens: ["mortal kombat", "shaolin"],
        runtimeName: "OpenS4kaXbox",
        nativeRuntimeAvailable: true
    )
    let digimon = Game(
        id: "digimon-rumble-arena-2-xbox",
        title: "Digimon Rumble Arena 2",
        subtitle: "Digital Monsters",
        coverResource: "digimon-rumble-arena-2",
        folderNames: ["Digimon Rumble Arena 2", "DigimonRumbleArena2"],
        nameTokens: ["digimon", "rumble arena 2"],
        runtimeName: "OpenS4kaDigimon",
        nativeRuntimeAvailable: true
    )
    let mostWanted = Game(
        id: "need-for-speed-most-wanted-xbox",
        title: "Need for Speed",
        subtitle: "Most Wanted",
        coverResource: "nfs-most-wanted",
        folderNames: ["Need for Speed Most Wanted", "Need for Speed - Most Wanted", "NFS Most Wanted", "NeedForSpeedMostWanted"],
        nameTokens: ["need for speed", "most wanted"],
        runtimeName: "OpenS4kaNFSMW",
        nativeRuntimeAvailable: true
    )
    var games: [Game] { [mortalKombat, digimon, mostWanted] }

    @Published private(set) var gamesDirectory: URL
    @Published private(set) var mountedGameDirectory: URL?
    @Published private(set) var mountedDirectories: [String: URL] = [:]
    @Published private(set) var folderAvailable = false
    @Published private(set) var message = ""
    /// false until the user picks a library folder (the onboarding asks for one)
    @Published private(set) var hasChosenFolder: Bool

    init(defaults: UserDefaults = .standard) {
        if let saved = defaults.string(forKey: "opens4ka.libraryFolder") {
            gamesDirectory = URL(fileURLWithPath: saved, isDirectory: true)
            hasChosenFolder = true
        } else {
            gamesDirectory = FileManager.default.homeDirectoryForCurrentUser  // where the folder panel opens
            hasChosenFolder = false
        }
        refresh()
    }

    var hasGame: Bool { !mountedDirectories.isEmpty }

    func directory(for game: Game) -> URL? {
        mountedDirectories[game.id]
    }

    var mountError: String {
        mountError(for: mortalKombat)
    }

    func mountError(for game: Game) -> String {
        if !hasChosenFolder {
            return "Folder library belum dipilih. Pilih di Settings → Folder game."
        }
        if !folderAvailable {
            return "Folder game tidak tersedia. Sambungkan drive atau pilih folder di Settings.\n\n\(gamesDirectory.path)"
        }
        return "\(game.title) belum ditemukan. Taruh ISO Xbox atau folder hasil ekstraknya di folder library, lalu scan ulang.\n\n\(gamesDirectory.path)"
    }

    func refresh() {
        var isDirectory: ObjCBool = false
        folderAvailable = hasChosenFolder
            && FileManager.default.fileExists(atPath: gamesDirectory.path, isDirectory: &isDirectory)
            && isDirectory.boolValue && FileManager.default.isReadableFile(atPath: gamesDirectory.path)
        let mounted = folderAvailable
            ? Dictionary(uniqueKeysWithValues: games.compactMap { game in
                resolveGameDirectory(for: game).map { (game.id, $0) }
            })
            : [:]
        mountedDirectories = mounted
        mountedGameDirectory = mounted[mortalKombat.id]
        message = !hasChosenFolder ? "Belum ada folder library"
            : hasGame ? "\(mounted.count) game ditemukan" : (folderAvailable ? "Game belum ditemukan" : "Folder tidak tersedia")
    }

    func openFolder() {
        refresh()
        if folderAvailable { NSWorkspace.shared.open(gamesDirectory) }
    }

    func chooseFolder() {
        let panel = NSOpenPanel()
        panel.title = "Pilih folder library OpenS4ka"
        panel.message = "Satu folder untuk semua game. Simpan tiap ISO di subfolder game masing-masing."
        panel.prompt = "Gunakan folder"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.directoryURL = gamesDirectory
        guard panel.runModal() == .OK, let selected = panel.url else { return }
        gamesDirectory = selected.standardizedFileURL
        hasChosenFolder = true
        UserDefaults.standard.set(gamesDirectory.path, forKey: "opens4ka.libraryFolder")
        refresh()
    }

    private func resolveGameDirectory(for game: Game) -> URL? {
        let manager = FileManager.default
        let children = (try? manager.contentsOfDirectory(
            at: gamesDirectory, includingPropertiesForKeys: [.isDirectoryKey], options: [.skipsHiddenFiles]
        )) ?? []
        let namedDirectories = children.filter { item in
            let name = item.lastPathComponent.lowercased()
            return (try? item.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true
                && game.nameTokens.contains(where: name.contains)
        }
        // Prefer the per-game folder so another port's disc never wins a runtime's ISO scan.
        let candidates = game.folderNames.map { gamesDirectory.appendingPathComponent($0, isDirectory: true) }
            + namedDirectories.sorted { $0.path < $1.path } + [gamesDirectory]
        var visited = Set<String>()
        return candidates.first { directory in
            let key = directory.standardizedFileURL.path
            guard visited.insert(key).inserted else { return false }
            let files = (try? manager.contentsOfDirectory(
                at: directory, includingPropertiesForKeys: [.isRegularFileKey], options: [.skipsHiddenFiles]
            )) ?? []
            let images = files.filter {
                ["iso", "xiso"].contains($0.pathExtension.lowercased())
                    && (try? $0.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true
            }
            let hasExecutable = files.contains {
                $0.lastPathComponent == "default.xbe"
                    && (try? $0.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true
            }
            let folderName = directory.lastPathComponent.lowercased()
            let namedForGame = game.nameTokens.contains(where: folderName.contains)
            let discName = images.first?.lastPathComponent.lowercased() ?? ""
            let discMatchesGame = game.nameTokens.contains(where: discName.contains)
            return hasExecutable || (images.count == 1 && (namedForGame || discMatchesGame))
        }
    }
}
