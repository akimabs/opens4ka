import SwiftUI
import AppKit

/// The first run: what OpenS4ka needs, where the library lives, and the controller. Shown until finished once;
/// Settings can bring it back.
struct OnboardingView: View {
    @ObservedObject var library: GameLibrary
    @ObservedObject var runtime: RuntimeController
    let onFinish: () -> Void
    @State private var step = 0
    private let steps = ["Welcome", "Game Library", "Controller", "Ready"]

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 10) {
                ForEach(steps.indices, id: \.self) { index in
                    Capsule()
                        .fill(.white.opacity(index <= step ? 0.85 : 0.18))
                        .frame(width: index == step ? 34 : 18, height: 4)
                }
                Spacer()
                Text("\(step + 1) / \(steps.count)").font(.system(size: 13)).foregroundStyle(.white.opacity(0.5))
            }
            .padding(.bottom, 36)

            Group {
                switch step {
                case 0: welcome
                case 1: libraryStep
                case 2: controllerStep
                default: doneStep
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)

            HStack(spacing: 14) {
                if step > 0 {
                    Button("Back") { step -= 1 }.buttonStyle(ConsoleButtonStyle())
                }
                Spacer()
                Text("✕ next   ○ back").font(.system(size: 12)).foregroundStyle(.white.opacity(0.45))
                Button(primaryLabel) { next() }
                    .buttonStyle(ConsoleButtonStyle())
                    .disabled(blocked)
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(56)
        .frame(maxWidth: 860, maxHeight: 620)
        .background(.black.opacity(0.72), in: RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14).stroke(.white.opacity(0.14)))
        .onReceive(runtime.launcherActions) { action in
            switch action {
            case .play: if blocked { library.chooseFolder() } else { next() }
            case .right: next()
            case .back, .left: if step > 0 { step -= 1 }
            default: break
            }
        }
        .onAppear { library.refresh() }
    }

    /// the controller is optional: without one the step is skipped, not completed
    private var primaryLabel: String {
        if step == steps.count - 1 { return "Start Playing" }
        if step == 2 && !runtime.controllerConnected { return "Skip" }
        return "Next"
    }

    /// the library step waits for a folder
    private var blocked: Bool { step == 1 && !library.hasChosenFolder }

    private func next() {
        guard !blocked else { return }
        if step < steps.count - 1 { step += 1 } else { onFinish() }
    }

    private func header(_ title: String, _ text: String) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(title).font(.system(size: 34, weight: .regular))
            Text(text).font(.system(size: 16)).foregroundStyle(.white.opacity(0.7)).lineSpacing(6)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var welcome: some View {
        VStack(alignment: .leading, spacing: 28) {
            LogoMark(size: 128)
            welcomeText
        }
    }

    private var welcomeText: some View {
        header("OpenS4ka", "A native macOS launcher for recompiled classic Xbox games. No emulator: each game "
               + "runs directly on Apple Silicon with Vulkan.")
    }

    private var libraryStep: some View {
        VStack(alignment: .leading, spacing: 22) {
            header("Game Library", "Choose or create a folder for your games. An external drive works too.")
            Text(library.hasChosenFolder ? library.gamesDirectory.path : "Not selected")
                .font(.system(size: 14, design: .monospaced)).lineLimit(2)
                .padding(16).frame(maxWidth: .infinity, alignment: .leading)
                .background(.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 8))
            Button("Choose / Create Folder") { library.chooseFolder() }.buttonStyle(ConsoleButtonStyle())
        }
    }

    private var controllerStep: some View {
        VStack(alignment: .leading, spacing: 22) {
            header("Connect Controller", "Optional—you can skip this step. DualShock 4 works over Bluetooth or USB; "
                   + "other macOS-supported controllers work too.")
            HStack(spacing: 12) {
                Image(systemName: "gamecontroller.fill").font(.system(size: 22))
                Text(runtime.controllerSummary).font(.system(size: 16))
                Text(runtime.controllerConnected ? "Connected" : "Offline").font(.system(size: 13))
                    .foregroundStyle(runtime.controllerConnected ? .green : .orange)
            }
            Text("1. Hold SHARE + PS until the controller light starts blinking.\n2. Open Mac Bluetooth settings and "
                 + "select Wireless Controller.\n3. Once connected, the controller is ready to use.")
                .font(.system(size: 15)).lineSpacing(8).foregroundStyle(.white.opacity(0.8))
            Button("Open Bluetooth Settings") { runtime.openBluetoothSettings() }.buttonStyle(ConsoleButtonStyle())
        }
    }

    private var doneStep: some View {
        header("Ready to Play", "You can change the library location and controller later in Settings.")
    }
}
