import SwiftUI
import AppKit

private enum SettingsCategory: String, CaseIterable {
    case cheats = "Cheat"
    case filter = "Filter"
    case antialiasing = "Anti-aliasing"
    case display = "Tampilan"
    case controller = "Input"
    case library = "Folder game"
}

struct SettingsHome: View {
    @ObservedObject var library: GameLibrary
    @ObservedObject var runtime: RuntimeController
    let onBackToGames: () -> Void
    @State private var category: SettingsCategory = .cheats
    @State private var inDetail = false
    @State private var focusedOption = 0
    @State private var capturing: String?  // the action waiting for a key
    @State private var keyMonitor: Any?

    var body: some View {
        GeometryReader { viewport in
            // Reserve the same title/footer space for every category. A tall
            // preview must only increase the scroll content, never this page.
            let contentHeight = max(0, viewport.size.height - 116)
            VStack(alignment: .leading, spacing: 28) {
                Text("Settings").font(.system(size: 36, weight: .regular))
                    .frame(height: 44, alignment: .leading)
                HStack(alignment: .top, spacing: 48) {
                    VStack(alignment: .leading, spacing: 5) {
                        ForEach(SettingsCategory.allCases, id: \.self) { item in
                            Button { category = item; inDetail = false } label: {
                                Text(item.rawValue)
                                    .font(.system(size: 21, weight: .regular))
                                    .foregroundStyle(.white.opacity(category == item ? 1 : 0.52))
                                    .padding(.horizontal, 20)
                                    .frame(maxWidth: .infinity, minHeight: 52, alignment: .leading)
                                    .background(.white.opacity(category == item ? 0.075 : 0), in: RoundedRectangle(cornerRadius: 3))
                                    .overlay {
                                        if category == item && !inDetail {
                                            RoundedRectangle(cornerRadius: 3).stroke(.white.opacity(0.78), lineWidth: 1.5)
                                        }
                                    }
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    .frame(width: 215)
                    Rectangle().fill(.white.opacity(0.14)).frame(width: 1).padding(.vertical, 4)
                    ConsoleScrollView(axis: .vertical, focusedItem: inDetail ? focusedOption : nil) {
                        details
                            .frame(maxWidth: .infinity, alignment: .topLeading)
                            .padding(.top, 10)
                            .padding(.horizontal, 2)
                            .padding(.bottom, 12)
                    }
                    .id(category)
                    .frame(width: max(0, viewport.size.width - 312), height: contentHeight, alignment: .topLeading)
                    .clipped()
            }
            .frame(width: viewport.size.width, height: contentHeight, alignment: .topLeading)
            Text(capturing != nil ? "Tekan tombol keyboard untuk aksi ini     ○  Batal"
                 : inDetail ? "↑ ↓  Pilih opsi     ×  Terapkan     ○  Kembali ke daftar" : "↑ ↓  Pilih kategori     ×  Buka     ○  Games")
                .font(.system(size: 12)).foregroundStyle(.white.opacity(0.6))
                .frame(height: 16, alignment: .leading)
            }
            .frame(width: viewport.size.width, height: viewport.size.height, alignment: .topLeading)
        }
        .foregroundStyle(.white)
        .shadow(color: .black.opacity(0.25), radius: 5, y: 2)
        .background {
            // Keep the same artwork, dimmed behind settings for readable text.
            Rectangle().fill(.black.opacity(0.38)).blur(radius: 35).padding(-35)
        }
        .onChange(of: category) { _ in stopCapture() }
        .onDisappear { stopCapture() }
        .onReceive(runtime.launcherActions) { action in
            guard UserDefaults.standard.bool(forKey: "opens4ka.onboarded"),
                  NSApp.keyWindow?.attachedSheet == nil, NSApp.modalWindow == nil else { return }
            switch action {
            case .up: move(-1)
            case .down: move(1)
            case .left: if inDetail { move(-1) }
            case .right: if inDetail { move(1) } else { enterDetail() }
            case .play: confirm()
            case .back: back()
            default: break
            }
        }
        .background {
            // Native shortcuts work without stealing focus from a folder dialog.
            Group {
                Button("") { move(-1) }.keyboardShortcut(.upArrow, modifiers: [])
                Button("") { move(1) }.keyboardShortcut(.downArrow, modifiers: [])
                Button("") { if inDetail { move(-1) } }.keyboardShortcut(.leftArrow, modifiers: [])
                Button("") { if inDetail { move(1) } else { enterDetail() } }.keyboardShortcut(.rightArrow, modifiers: [])
                Button("") { confirm() }.keyboardShortcut(.return, modifiers: [])
                Button("") { back() }.keyboardShortcut(.escape, modifiers: [])
            }.hidden().accessibilityHidden(true)
        }
    }

    @ViewBuilder private var details: some View {
        switch category {
        case .cheats:
            // one card per game; only Mortal Kombat has cheats so far
            section("Mortal Kombat: Shaolin Monks") { choiceDetail(title: "Buka semua karakter", description: "Semua karakter bisa dipilih di character select. Berlaku saat game dibuka berikutnya.",
                         selection: Binding(get: { runtime.unlockCharacters ? 1 : 0 }, set: { runtime.unlockCharacters = $0 == 1 }),
                         values: [(0, "Off"), (1, "On")]) }
        case .filter:
            section("Gameplay") { choiceDetail(title: "Filter layar", description: "Pilih tampilan gameplay. CRT memberi efek scanline dan warna TV jadul. Efek CRT pada launcher selalu aktif.",
                         selection: $runtime.screenFilter,
                         values: [(0, "Off"), (1, "Sharpen"), (2, "Vivid"), (3, "Scanlines"), (4, "CRT")]) }
        case .antialiasing:
            section("Kualitas") { choiceDetail(title: "Anti-aliasing", description: "Haluskan tepi objek saat bermain. SSAA 2× membutuhkan lebih banyak daya GPU. Berlaku saat game dibuka berikutnya.",
                         selection: $runtime.antiAliasing, values: [(0, "Off"), (1, "FXAA"), (2, "SSAA 2×")]) }
        case .display:
            VStack(alignment: .leading, spacing: 18) {
                section("Layar") { displayDetail }
                section("Kamera") {
                Button {
                    runtime.ultrawide.toggle()
                    focusedOption = 5
                    inDetail = true
                } label: {
                    HStack {
                        Text("Kamera ultrawide")
                        Spacer()
                        Text(runtime.ultrawide ? "On" : "Off")
                    }
                }
                    .buttonStyle(.plain).font(.system(size: 17)).padding(12)
                    .overlay(RoundedRectangle(cornerRadius: 3).stroke(.white.opacity(inDetail && focusedOption == 5 ? 0.8 : 0), lineWidth: 1.5))
                    .consoleScrollTarget(5)
                }
            }
        case .controller:
            VStack(alignment: .leading, spacing: 18) {
                section("Controller") {
                    HStack(spacing: 12) {
                        Text(runtime.controllerKind).font(.system(size: 19))
                        Text(runtime.controllerConnected ? "Connected" : "Offline")
                            .font(.system(size: 13)).foregroundStyle(runtime.controllerConnected ? .green : .orange)
                    }
                    Text("DualSense atau DualShock 4: tahan Create/Share + PS sampai lampu berkedip, lalu pilih di "
                         + "Bluetooth Mac. Kabel USB juga bisa.")
                        .font(.system(size: 14)).lineSpacing(6).foregroundStyle(.white.opacity(0.7))
                        .fixedSize(horizontal: false, vertical: true)
                    actionButton("Buka Bluetooth", index: 0, action: runtime.openBluetoothSettings)
                    Divider().overlay(.white.opacity(0.12))
                    Button {
                        runtime.playstationPrompts.toggle()
                        focusedOption = 1
                        inDetail = true
                    } label: {
                        HStack {
                            VStack(alignment: .leading, spacing: 4) {
                                Text("Use PS2 button").font(.system(size: 17))
                                Text("Ikon tombol di game jadi ✕ ○ □ △ dan L1/R1/L2/R2")
                                    .font(.system(size: 12)).foregroundStyle(.white.opacity(0.55))
                            }
                            Spacer()
                            Text(runtime.playstationPrompts ? "On" : "Off").font(.system(size: 17))
                        }
                        .padding(10).contentShape(Rectangle())
                    }
                        .buttonStyle(.plain)
                        .overlay(RoundedRectangle(cornerRadius: 3).stroke(.white.opacity(inDetail && focusedOption == 1 ? 0.8 : 0), lineWidth: 1.5))
                        .consoleScrollTarget(1)
                }
                section("Keyboard", note: "Pilih aksi lalu tekan tombol baru. Tombol yang sudah dipakai aksi lain ditukar. "
                        + "Mouse menggerakkan kamera.") {
                    keyboardMap
                    actionButton("Reset ke default", index: 2 + KeyBindings.actions.count, action: runtime.resetKeys)
                        .padding(.top, 6)
                }
            }
        case .library:
            VStack(alignment: .leading, spacing: 18) {
                section("Lokasi library", note: "Satu folder untuk semua game. Simpan tiap ISO di subfolder game masing-masing, atau pilih folder yang langsung berisi satu ISO.") {
                VStack(alignment: .leading, spacing: 12) {
                    Text(library.gamesDirectory.path)
                        .font(.system(size: 14, design: .monospaced)).lineLimit(3)
                    Text(library.message).font(.system(size: 14)).foregroundStyle(library.hasGame ? .green : .orange)
                }
                .padding(16).frame(maxWidth: .infinity, alignment: .leading)
                .background(.black.opacity(0.3), in: RoundedRectangle(cornerRadius: 6))
                HStack(spacing: 14) {
                    actionButton("Pilih folder", index: 0, action: library.chooseFolder)
                    actionButton("Scan ulang", index: 1, action: library.refresh)
                    actionButton("Buka di Finder", index: 2, action: library.openFolder).disabled(!library.folderAvailable)
                }
                Text("Game dibaca langsung dari folder ini. Folder atau drive yang terputus akan menampilkan error saat Play.")
                    .font(.system(size: 13)).foregroundStyle(.white.opacity(0.5)).lineSpacing(5)
                }
                section("Onboarding") {
                    actionButton("Ulangi onboarding", index: 3) { UserDefaults.standard.set(false, forKey: "opens4ka.onboarded") }
                }
            }
        }
    }


    /// one row per action: the pad's name for it and its keys; ✕ or a click waits for the new key
    private var keyboardMap: some View {
        VStack(spacing: 2) {
            ForEach(Array(KeyBindings.actions.enumerated()), id: \.element.id) { index, action in
                let option = 2 + index
                Button { startCapture(action.id, option: option) } label: {
                    HStack {
                        Text(runtime.playstationPrompts ? action.playstation : action.xbox).font(.system(size: 15))
                        Spacer()
                        Text(capturing == action.id ? "Tekan tombol…" : KeyBindings.label(runtime.keyBindings[action.id] ?? ""))
                            .font(.system(size: 14, design: .monospaced))
                            .foregroundStyle(capturing == action.id ? Color.accentColor : .white.opacity(0.85))
                    }
                    .padding(.horizontal, 12).frame(height: 34)
                    .background(.white.opacity(index % 2 == 0 ? 0.035 : 0), in: RoundedRectangle(cornerRadius: 4))
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .overlay(RoundedRectangle(cornerRadius: 4)
                    .stroke(.white.opacity(inDetail && focusedOption == option ? 0.8 : 0), lineWidth: 1.5))
                .consoleScrollTarget(option)
            }
        }
    }

    private func startCapture(_ action: String, option: Int) {
        focusedOption = option
        inDetail = true
        if capturing == action { stopCapture(); return }
        stopCapture()
        capturing = action
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .flagsChanged]) { event in
            guard let id = capturing else { return event }
            if let name = KeyBindings.name(for: event) {
                runtime.bind(id, to: name)
                stopCapture()
            } else if event.type == .keyDown {
                NSSound.beep()
            }
            return nil  // the key is the binding, not navigation
        }
    }

    private func stopCapture() {
        if let keyMonitor { NSEvent.removeMonitor(keyMonitor) }
        keyMonitor = nil
        capturing = nil
    }

    /// A titled card: each part of a setting's page apart from the next.
    private func section<Content: View>(_ title: String, note: String? = nil,
                                        @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            VStack(alignment: .leading, spacing: 6) {
                Text(title.uppercased()).font(.system(size: 12, weight: .semibold)).tracking(1.5)
                    .foregroundStyle(.white.opacity(0.55))
                if let note {
                    Text(note).font(.system(size: 13)).foregroundStyle(.white.opacity(0.55)).lineSpacing(4)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            content()
        }
        .padding(20)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.white.opacity(0.045), in: RoundedRectangle(cornerRadius: 10))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(.white.opacity(0.1)))
    }

    private func detailHeader(_ title: String, description: String) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(title).font(.system(size: 26, weight: .regular))
            Text(description).font(.system(size: 16)).foregroundStyle(.white.opacity(0.65)).lineSpacing(5)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private func choiceDetail(title: String, description: String, selection: Binding<UInt32>, values: [(UInt32, String)]) -> some View {
        HStack(alignment: .top, spacing: 28) {
            detailHeader(title, description: description).frame(maxWidth: .infinity, alignment: .leading)
            optionList(selection: selection, values: values)
        }
    }

    private var displayDetail: some View {
        HStack(alignment: .top, spacing: 28) {
            VStack(alignment: .leading, spacing: 16) {
                detailHeader("Aspect ratio", description: "Atur lebar layar game. Kamera ultrawide memperluas pandangan; HUD tetap dalam area 4:3.")
                DisplayPreview(aspectRatio: runtime.aspectRatio, ultrawide: runtime.ultrawide)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            optionList(selection: $runtime.aspectRatio,
                       values: [(0, "Full screen"), (133, "4:3"), (160, "16:10"), (178, "16:9 Wide"), (200, "18:9")])
        }
    }

    private func optionList(selection: Binding<UInt32>, values: [(UInt32, String)]) -> some View {
        VStack(spacing: 0) {
            ForEach(values, id: \.0) { value in
                Button {
                    selection.wrappedValue = value.0
                    focusedOption = values.firstIndex(where: { $0.0 == value.0 }) ?? 0
                    inDetail = true
                } label: {
                    HStack(spacing: 12) {
                        Text(selection.wrappedValue == value.0 ? "✓" : " ").frame(width: 15)
                        Text(value.1)
                        Spacer(minLength: 0)
                    }
                    .font(.system(size: 20, weight: .regular))
                    .padding(.horizontal, 18).frame(height: 52)
                    .background(.white.opacity(selection.wrappedValue == value.0 ? 0.08 : 0))
                    .overlay {
                        if inDetail && values.firstIndex(where: { $0.0 == value.0 }) == focusedOption {
                            RoundedRectangle(cornerRadius: 3).stroke(.white.opacity(0.8), lineWidth: 1.5)
                        }
                    }
                }
                .buttonStyle(.plain)
                .accessibilityLabel("\(value.1)\(selection.wrappedValue == value.0 ? ", selected" : "")")
                .consoleScrollTarget(values.firstIndex(where: { $0.0 == value.0 }) ?? 0)
            }
        }
        .frame(width: 200).background(.black.opacity(0.4))
        .padding(2)
    }

    private func actionButton(_ title: String, index: Int, action: @escaping () -> Void) -> some View {
        Button {
            inDetail = true
            focusedOption = index
            action()
        } label: { Text(title) }
        .buttonStyle(ConsoleButtonStyle())
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(.white.opacity(inDetail && focusedOption == index ? 0.8 : 0), lineWidth: 1.5))
        .consoleScrollTarget(index)
    }

    private var optionCount: Int {
        switch category {
        case .cheats: return 2
        case .filter: return 5
        case .antialiasing: return 3
        case .library: return 4
        case .display: return 6
        case .controller: return 3 + KeyBindings.actions.count
        }
    }

    private func enterDetail() {
        switch category {
        case .cheats: focusedOption = runtime.unlockCharacters ? 1 : 0
        case .filter: focusedOption = min(Int(runtime.screenFilter), 4)
        case .antialiasing: focusedOption = min(Int(runtime.antiAliasing), 2)
        case .display: focusedOption = [UInt32(0), 133, 160, 178, 200].firstIndex(of: runtime.aspectRatio) ?? 0
        case .controller, .library: focusedOption = 0
        }
        inDetail = true
    }

    private func back() {
        if capturing != nil { stopCapture(); return }
        if inDetail { inDetail = false } else { onBackToGames() }
    }

    private func move(_ direction: Int) {
        if inDetail {
            focusedOption = min(max(focusedOption + direction, 0), optionCount - 1)
            return
        }
        let categories = SettingsCategory.allCases
        let current = categories.firstIndex(of: category) ?? 0
        category = categories[min(max(current + direction, 0), categories.count - 1)]
    }

    private func confirm() {
        guard inDetail else { enterDetail(); return }
        // Confirming an option deliberately keeps focus in this submenu until Circle / Escape.
        switch category {
        case .cheats: runtime.unlockCharacters = focusedOption == 1
        case .filter: runtime.screenFilter = UInt32(focusedOption)
        case .antialiasing: runtime.antiAliasing = UInt32(focusedOption)
        case .display:
            if focusedOption == 5 { runtime.ultrawide.toggle() }
            else { runtime.aspectRatio = [0, 133, 160, 178, 200][focusedOption] }
        case .controller:
            switch focusedOption {
            case 0: runtime.openBluetoothSettings()
            case 1: runtime.playstationPrompts.toggle()
            case 2 + KeyBindings.actions.count: runtime.resetKeys()
            default: startCapture(KeyBindings.actions[focusedOption - 2].id, option: focusedOption)
            }
        case .library:
            switch focusedOption {
            case 0: library.chooseFolder()
            case 1: library.refresh()
            case 2: library.openFolder()
            default: UserDefaults.standard.set(false, forKey: "opens4ka.onboarded")
            }
        }
    }
}

private struct DisplayPreview: View {
    let aspectRatio: UInt32
    let ultrawide: Bool

    private var screenRatio: CGFloat {
        guard let screen = NSApp.keyWindow?.screen ?? NSScreen.main else { return 16.0 / 9.0 }
        return screen.frame.width / max(screen.frame.height, 1)
    }

    private var aspectLabel: String {
        switch aspectRatio {
        case 133: return "4:3"
        case 160: return "16:10"
        case 178: return "16:9 WIDE"
        case 200: return "18:9"
        default: return "FULL SCREEN"
        }
    }

    private var selectedRatio: CGFloat? {
        switch aspectRatio {
        case 133: return 4.0 / 3.0
        case 160: return 16.0 / 10.0
        case 178: return 16.0 / 9.0
        case 200: return 2.0
        default: return nil
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Preview ukuran layar game")
                .font(.system(size: 18, weight: .medium))
            VStack(alignment: .leading, spacing: 12) {
                GeometryReader { geometry in
                    let screenRatio = geometry.size.width / max(geometry.size.height, 1)
                    let frameRatio = selectedRatio ?? screenRatio
                    let frameWidth = frameRatio >= screenRatio
                        ? geometry.size.width
                        : geometry.size.height * frameRatio
                    let frameHeight = frameWidth / frameRatio
                    ZStack {
                        RoundedRectangle(cornerRadius: 6)
                            .fill(.black.opacity(0.82))
                            .overlay(RoundedRectangle(cornerRadius: 6).stroke(.white.opacity(0.22), lineWidth: 1))
                        RoundedRectangle(cornerRadius: 4)
                            .fill(Color(red: 0.18, green: 0.22, blue: 0.22))
                            .frame(width: frameWidth, height: frameHeight)
                            .overlay {
                                Canvas { context, size in
                                    var path = Path()
                                    for y in stride(from: CGFloat.zero, to: size.height, by: 4) {
                                        path.move(to: CGPoint(x: 0, y: y))
                                        path.addLine(to: CGPoint(x: size.width, y: y))
                                    }
                                    context.stroke(path, with: .color(.black.opacity(0.18)), lineWidth: 1)
                                }
                                .clipShape(RoundedRectangle(cornerRadius: 4))
                            }
                            .overlay {
                                Text(aspectLabel)
                                    .font(.system(size: 15, weight: .semibold, design: .rounded))
                                    .tracking(1.2)
                                    .foregroundStyle(.white.opacity(0.88))
                            }
                            .overlay {
                                if ultrawide && (selectedRatio ?? screenRatio) > 4.0 / 3.0 {
                                    RoundedRectangle(cornerRadius: 3)
                                        .stroke(.white.opacity(0.48), style: StrokeStyle(lineWidth: 1, dash: [5, 4]))
                                        .frame(width: min(frameWidth * 0.74, frameHeight * 4.0 / 3.0),
                                               height: min(frameHeight * 0.74, frameWidth * 3.0 / 4.0))
                                }
                            }
                    }
                }
                .aspectRatio(screenRatio, contentMode: .fit)
                .frame(maxWidth: 340)

                VStack(alignment: .leading, spacing: 8) {
                    Text(aspectLabel)
                        .font(.system(size: 22, weight: .semibold))
                    Text(aspectRatio == 0 ? "Game memenuhi layar penuh." : "Area hitam menunjukkan letterbox.")
                        .font(.system(size: 14))
                        .foregroundStyle(.white.opacity(0.68))
                        .fixedSize(horizontal: false, vertical: true)
                    if ultrawide && (selectedRatio ?? 16.0 / 9.0) > 4.0 / 3.0 {
                        Text("Garis putus-putus = area HUD 4:3")
                            .font(.system(size: 12))
                            .foregroundStyle(.white.opacity(0.52))
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.black.opacity(0.35), in: RoundedRectangle(cornerRadius: 7))
        .overlay(RoundedRectangle(cornerRadius: 7).stroke(.white.opacity(0.16), lineWidth: 1))
    }
}
