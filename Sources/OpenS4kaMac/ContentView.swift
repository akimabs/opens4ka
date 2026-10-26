import SwiftUI
import AppKit

private let accent = Color(red: 0.56, green: 0.88, blue: 0.94)
private enum LauncherTab { case games, settings }

struct ContentView: View {
    @ObservedObject var library: GameLibrary
    @ObservedObject var runtime: RuntimeController
    @State private var selectedTab: LauncherTab = .games
    @State private var selectedGameIndex = 0
    @State private var gameEdgeSequence = 0
    @State private var gameEdgeDirection = 0
    @State private var showError = false
    @State private var errorTitle = ""
    @State private var errorMessage = ""
    @AppStorage("opens4ka.onboarded") private var onboarded = false

    var body: some View {
        GeometryReader { geometry in
            let inset = max(40, geometry.size.width * 0.048)
            VStack(spacing: 0) {
                TopBar(runtime: runtime)
                    .frame(height: 48)
                    .padding(.bottom, selectedTab == .games ? geometry.size.height * 0.085 : 34)

                Group {
                    if selectedTab == .games {
                        GamesHome(library: library, selectedIndex: $selectedGameIndex,
                                  edgeAttempt: gameEdgeAttempt, onPlay: playSelectedGame,
                                  onMoveSelection: moveGameSelection)
                    } else {
                        SettingsHome(library: library, runtime: runtime, onBackToGames: { selectedTab = .games })
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)

                BottomNavigation(selectedTab: $selectedTab)
                    .padding(.top, 18)
            }
            .padding(.horizontal, inset)
            .padding(.top, 34)
            .padding(.bottom, 28)
            .frame(width: geometry.size.width, height: geometry.size.height)
            .background { LibraryBackdrop().frame(width: geometry.size.width, height: geometry.size.height).clipped() }
            .overlay {
                if !onboarded {
                    ZStack {
                        Color.black.opacity(0.55)
                        OnboardingView(library: library, runtime: runtime) { onboarded = true }
                            .padding(40)
                    }
                }
            }
            .overlay { CRTOverlay().allowsHitTesting(false) }
            .modifier(CRTScreen(size: geometry.size))
            .clipped()
        }
        .frame(minWidth: 1000, minHeight: 680)
        .preferredColorScheme(.dark)
        .tint(accent)
        .onAppear { runtime.refresh(); library.refresh() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            library.refresh()
        }
        .onReceive(runtime.launcherActions) { action in
            guard onboarded, !showError, NSApp.keyWindow?.attachedSheet == nil else { return }
            switch action {
            case .games: selectedTab = .games
            case .settings: selectedTab = .settings
            case .play: if selectedTab == .games { playSelectedGame() }
            case .left: if selectedTab == .games { moveGameSelection(-1) }
            case .right: if selectedTab == .games { moveGameSelection(1) }
            case .back, .up, .down: break
            }
        }
        .alert(errorTitle, isPresented: $showError) {
            Button("Buka Settings") { selectedTab = .settings }
            Button("OK", role: .cancel) {}
        } message: { Text(errorMessage) }
    }

    private func playSelectedGame() {
        library.refresh()
        guard library.games.indices.contains(selectedGameIndex) else { return }
        let game = library.games[selectedGameIndex]
        guard let gameDirectory = library.directory(for: game) else {
            presentError("Game belum ter-mount", message: library.mountError(for: game))
            return
        }
        guard game.nativeRuntimeAvailable else {
            presentError("Runtime belum tersedia", message: "ISO \(game.title) sudah terdaftar di library, tetapi native runtime-nya belum dipasang di app ini.")
            return
        }
        guard runtime.isAvailable(for: game.runtimeName) else {
            presentError("Runtime belum tersedia", message: "Runtime Xbox native untuk \(game.title) belum terpasang di app ini.")
            return
        }
        if !runtime.play(gameDirectory: gameDirectory, gameTitle: game.title, runtimeName: game.runtimeName) {
            presentError("Game gagal dibuka", message: runtime.status)
        }
    }

    private func presentError(_ title: String, message: String) {
        errorTitle = title
        errorMessage = message
        showError = true
    }

    private func moveGameSelection(_ direction: Int) {
        guard !library.games.isEmpty else { return }
        let nextIndex = selectedGameIndex + direction
        guard (0..<library.games.count).contains(nextIndex) else {
            // Selection stays clamped, but the list still gives the user the
            // same physical edge feedback as a native macOS scroll view.
            gameEdgeDirection = direction < 0 ? 1 : -1
            gameEdgeSequence += 1
            return
        }
        selectedGameIndex = nextIndex
    }

    private var gameEdgeAttempt: ConsoleScrollEdgeAttempt? {
        guard gameEdgeSequence > 0 else { return nil }
        return ConsoleScrollEdgeAttempt(sequence: gameEdgeSequence, direction: gameEdgeDirection)
    }
}

private struct TopBar: View {
    @ObservedObject var runtime: RuntimeController

    var body: some View {
        HStack(spacing: 18) {
            HStack(spacing: 12) {
                LogoMark(size: 34)
                Text("OpenS4ka")
                    .font(.system(size: 25, weight: .semibold, design: .rounded))
            }
            Spacer()
            Circle().fill(runtime.controllerConnected ? Color.green.opacity(0.9) : .white.opacity(0.4))
                .frame(width: 6, height: 6)
            Text(runtime.controllerConnected ? runtime.controllerKind : "Controller offline")
                .font(.system(size: 12, weight: .semibold))
                .tracking(1.2)
                .foregroundStyle(.white.opacity(0.8))
            TimelineView(.periodic(from: .now, by: 30)) { context in
                Text(context.date, format: .dateTime.hour().minute())
                    .font(.system(size: 26, weight: .medium))
                    .monospacedDigit()
            }
            .padding(.leading, 14)
        }
        .foregroundStyle(.white)
        .shadow(color: .black.opacity(0.4), radius: 8, y: 2)
    }
}

private struct GamesHome: View {
    @ObservedObject var library: GameLibrary
    @Binding var selectedIndex: Int
    let edgeAttempt: ConsoleScrollEdgeAttempt?
    let onPlay: () -> Void
    let onMoveSelection: (Int) -> Void

    var body: some View {
        GeometryReader { geometry in
            let titleSize = min(62, max(42, geometry.size.height * 0.105))
            let tileSize = min(310, max(170, geometry.size.height - titleSize * 1.2 - 90))
            let selectedGame = library.games.indices.contains(selectedIndex) ? library.games[selectedIndex] : nil
            VStack(alignment: .leading, spacing: 0) {
                Text(selectedGame?.title ?? "Games")
                    .font(.system(size: titleSize, weight: .bold))
                    .tracking(-1.4)
                    .foregroundStyle(.white)
                    .shadow(color: .black.opacity(0.25), radius: 6, y: 2)
                Text(selectedGame?.subtitle ?? "")
                    .font(.system(size: 18, weight: .medium))
                .foregroundStyle(.white.opacity(0.7))
                .padding(.top, 4)

                ConsoleScrollView(axis: .horizontal, focusedItem: selectedIndex, edgeAttempt: edgeAttempt) {
                    HStack(spacing: 22) {
                        ForEach(Array(library.games.enumerated()), id: \.element.id) { index, game in
                            GameCard(game: game, size: tileSize, selected: index == selectedIndex)
                                .consoleScrollTarget(index)
                                .contentShape(Rectangle())
                                .onTapGesture {
                                    selectedIndex = index
                                    onPlay()
                                }
                        }
                    }
                    .padding(.horizontal, 8)
                    .frame(minHeight: tileSize + 14, alignment: .top)
                }
                .padding(.top, 22)
                .frame(maxWidth: .infinity, minHeight: tileSize + 42, maxHeight: tileSize + 42,
                       alignment: .topLeading)

                // Keep arrow-key navigation in sync with the DS4 D-pad. The
                // controls are intentionally invisible; the console UI stays
                // clean while still giving edge bounce on desktop input.
                HStack(spacing: 0) {
                    Button("") { onMoveSelection(-1) }.keyboardShortcut(.leftArrow, modifiers: [])
                    Button("") { onMoveSelection(1) }.keyboardShortcut(.rightArrow, modifiers: [])
                }
                .frame(width: 0, height: 0)
                .opacity(0)

                Spacer(minLength: 0)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
    }
}

private struct GameCard: View {
    let game: GameLibrary.Game
    let size: CGFloat
    let selected: Bool
    @State private var hovered = false

    var body: some View {
        Artwork(resource: game.coverResource)
            .scaledToFill()
            .frame(width: size, height: size, alignment: .top)
            .clipped()
            .clipShape(RoundedRectangle(cornerRadius: 5))
            .overlay {
                RoundedRectangle(cornerRadius: 5)
                    .stroke(selected ? accent : .clear, lineWidth: selected ? 3 : 0)
            }
            .shadow(color: selected ? accent.opacity(0.52) : .black.opacity(0.4),
                    radius: selected ? 18 : 16, y: 8)
            .scaleEffect(hovered ? 1.018 : 1)
            .animation(.easeInOut(duration: 0.2), value: hovered)
            .animation(.easeInOut(duration: 0.2), value: selected)
            .onHover { hovered = $0 }
    }
}

private struct BottomNavigation: View {
    @Binding var selectedTab: LauncherTab

    var body: some View {
        HStack(spacing: 25) {
            Text("L1").font(.system(size: 12, weight: .bold, design: .rounded))
                .padding(.horizontal, 9).padding(.vertical, 5)
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(.white.opacity(0.65)))
            HStack(spacing: 0) {
                tab("Games", value: .games).keyboardShortcut("g", modifiers: [])
                Rectangle().fill(.white.opacity(0.16)).frame(width: 1, height: 24)
                tab("Settings", value: .settings).keyboardShortcut("s", modifiers: [])
            }
            .padding(.horizontal, 20)
            .background(.black.opacity(0.8), in: RoundedRectangle(cornerRadius: 23))
            .overlay(RoundedRectangle(cornerRadius: 23).stroke(.white.opacity(0.1), lineWidth: 1))
            Text("R1").font(.system(size: 12, weight: .bold, design: .rounded))
                .padding(.horizontal, 9).padding(.vertical, 5)
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(.white.opacity(0.65)))
        }
        .foregroundStyle(.white)
        .frame(maxWidth: .infinity)
    }

    private func tab(_ label: String, value: LauncherTab) -> some View {
        Button { selectedTab = value } label: {
            Text(label)
                .font(.system(size: 25, weight: selectedTab == value ? .semibold : .regular))
                .foregroundStyle(selectedTab == value ? .white : .white.opacity(0.5))
                .frame(width: 164, height: 62)
                .overlay(alignment: .bottom) {
                    if selectedTab == value {
                        Capsule().fill(accent).frame(width: 30, height: 2).padding(.bottom, 7)
                    }
                }
        }
        .buttonStyle(.plain)
    }
}

struct ConsoleButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 12, weight: .medium))
            .padding(.horizontal, 13).padding(.vertical, 9)
            .background(.white.opacity(configuration.isPressed ? 0.22 : 0.1), in: RoundedRectangle(cornerRadius: 6))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(.white.opacity(0.2), lineWidth: 1))
    }
}

@MainActor enum LauncherArtwork {
    // Decode once; clock/controller updates must not repeatedly decode the background.
    static let images: [String: NSImage] = {
        var result: [String: NSImage] = [:]
        let packaged = Bundle.main.resourceURL?.appendingPathComponent("OpenS4kaMac_OpenS4kaMac.bundle")
        let resourceDirectory: URL
        if let packaged, FileManager.default.fileExists(atPath: packaged.path) {
            resourceDirectory = packaged
        } else {
            resourceDirectory = Bundle.module.bundleURL
        }
        for name in ["mortal-kombat-ico", "digimon-rumble-arena-2", "nfs-most-wanted", "opens4ka-bg"] {
            let url = resourceDirectory.appendingPathComponent(name).appendingPathExtension("webp")
            if let image = NSImage(contentsOf: url) { result[name] = image }
        }
        if let logo = NSImage(contentsOf: resourceDirectory.appendingPathComponent("opens4ka-logo.png")) {
            result["opens4ka-logo"] = logo
        }
        return result
    }()
}

private struct Artwork: View {
    let resource: String
    var body: some View {
        if let image = LauncherArtwork.images[resource] {
            Image(nsImage: image).resizable()
        } else {
            Color(red: 0.1, green: 0.15, blue: 0.13)
        }
    }
}

/// The pixel logo, scaled by whole steps of its art so the pixels stay square.
struct LogoMark: View {
    let size: CGFloat
    var body: some View {
        if let image = LauncherArtwork.images["opens4ka-logo"] {
            Image(nsImage: image).interpolation(.none).resizable().frame(width: size, height: size)
        }
    }
}

private struct LibraryBackdrop: View {
    var body: some View {
        GeometryReader { geometry in
            Artwork(resource: "opens4ka-bg")
                .scaledToFill()
                .frame(width: geometry.size.width, height: geometry.size.height)
                .clipped()
                .overlay {
                    LinearGradient(colors: [.black.opacity(0.62), .black.opacity(0.30), .black.opacity(0.10)],
                                   startPoint: .leading, endPoint: .trailing)
                }
                .overlay {
                    LinearGradient(colors: [.black.opacity(0.12), .clear, .black.opacity(0.46)],
                                   startPoint: .top, endPoint: .bottom)
                }
        }
    }
}

private struct CRTOverlay: View {
    var body: some View {
        GeometryReader { geometry in
            Canvas { context, size in
                var lines = Path()
                for y in stride(from: CGFloat.zero, to: size.height, by: 3) {
                    lines.move(to: CGPoint(x: 0, y: y))
                    lines.addLine(to: CGPoint(x: size.width, y: y))
                }
                context.stroke(lines, with: .color(.black.opacity(0.14)), lineWidth: 1)
                for (offset, color) in [(CGFloat(0), Color.red), (1, Color.green), (2, Color.blue)] {
                    var phosphor = Path()
                    for x in stride(from: offset, to: size.width, by: 3) {
                        phosphor.move(to: CGPoint(x: x, y: 0))
                        phosphor.addLine(to: CGPoint(x: x, y: size.height))
                    }
                    context.stroke(phosphor, with: .color(color.opacity(0.018)), lineWidth: 1)
                }
            }
            .overlay {
                RadialGradient(colors: [.clear, .black.opacity(0.32)], center: .center,
                               startRadius: geometry.size.width * 0.28, endRadius: geometry.size.width * 0.7)
            }
        }
    }
}
