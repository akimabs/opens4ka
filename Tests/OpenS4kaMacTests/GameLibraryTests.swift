import Foundation
import Testing
@testable import OpenS4kaMac

private struct LibraryFixture {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("opens4ka-library-test-\(UUID())")
    let suite = "opens4ka-library-test-\(UUID())"
    var defaults: UserDefaults { UserDefaults(suiteName: suite)! }

    init() throws {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defaults.set(root.path, forKey: "opens4ka.libraryFolder")
    }

    func file(_ relative: String) throws {
        let url = root.appendingPathComponent(relative)
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data().write(to: url)
    }

    func cleanup() {
        try? FileManager.default.removeItem(at: root)
        defaults.removePersistentDomain(forName: suite)
    }
}

@Test @MainActor func existingLibraryUsesGameSubfolderWithoutMovingFiles() throws {
    let fixture = try LibraryFixture()
    defer { fixture.cleanup() }
    try fixture.file("Mortal Kombat/Shaolin Monks.xiso.iso")
    try fixture.file("Other Game.iso")
    let library = GameLibrary(defaults: fixture.defaults)
    #expect(library.hasGame)
    #expect(library.mountedGameDirectory == fixture.root.appendingPathComponent("Mortal Kombat", isDirectory: true))
}

@Test @MainActor func unavailableFolderIsNotRecreatedAndGameIsUnmounted() throws {
    let fixture = try LibraryFixture()
    defer { fixture.cleanup() }
    try fixture.file("Mortal Kombat/default.xbe")
    let library = GameLibrary(defaults: fixture.defaults)
    #expect(library.hasGame)
    try FileManager.default.removeItem(at: fixture.root)
    library.refresh()
    #expect(!library.hasGame)
    #expect(!library.folderAvailable)
    #expect(!FileManager.default.fileExists(atPath: fixture.root.path))
    #expect(library.mountError.contains(fixture.root.path))
}

@Test @MainActor func emptyAndUnrelatedDiscLibrariesDoNotMountMortalKombat() throws {
    let fixture = try LibraryFixture()
    defer { fixture.cleanup() }
    let library = GameLibrary(defaults: fixture.defaults)
    #expect(library.folderAvailable)
    #expect(!library.hasGame)
    try fixture.file("Another Game.iso")
    library.refresh()
    #expect(!library.hasGame)
}

@Test @MainActor func directXisoIsRecognizedAndAmbiguousDiscsRequireSubfolder() throws {
    let fixture = try LibraryFixture()
    defer { fixture.cleanup() }
    try fixture.file("Mortal Kombat.XISO")
    let library = GameLibrary(defaults: fixture.defaults)
    #expect(library.hasGame)
    #expect(library.mountedGameDirectory?.path == fixture.root.path)
    try fixture.file("Other Game.iso")
    library.refresh()
    #expect(!library.hasGame)
}

@Test @MainActor func digimonGameIsListedAndMountedSeparately() throws {
    let fixture = try LibraryFixture()
    defer { fixture.cleanup() }
    try fixture.file("Digimon Rumble Arena 2/Digimon Rumble Arena 2.xiso.iso")
    let library = GameLibrary(defaults: fixture.defaults)
    #expect(library.games.contains { $0.id == "digimon-rumble-arena-2-xbox" })
    #expect(library.directory(for: library.digimon) == fixture.root.appendingPathComponent("Digimon Rumble Arena 2", isDirectory: true))
    #expect(library.digimon.nativeRuntimeAvailable)
    #expect(library.digimon.runtimeName == "OpenS4kaDigimon")
}
