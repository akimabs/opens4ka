import SwiftUI

@main
struct OpenS4kaMacApp: App {
    @StateObject private var library = GameLibrary()
    @StateObject private var runtime = RuntimeController()

    var body: some Scene {
        WindowGroup("OpenS4ka") {
            ContentView(library: library, runtime: runtime)
        }
        .windowStyle(.hiddenTitleBar)
        .defaultSize(width: 1280, height: 800)
        .windowResizability(.contentSize)
    }
}
