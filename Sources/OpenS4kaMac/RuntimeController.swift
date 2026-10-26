import Foundation
import Combine
import AppKit
import GameController
import OpenS4kaRuntime

@MainActor
final class RuntimeController: ObservableObject {
    enum LauncherAction { case games, settings, play, back, up, down, left, right }
    let launcherActions = PassthroughSubject<LauncherAction, Never>()
    @Published private(set) var status = "macOS runtime not installed"
    @Published private(set) var controllerConnected = false
    @Published private(set) var controllerName = "No controller connected"
    @Published private(set) var controllerKind = "CONTROLLER"
    @Published var unlockCharacters: Bool {
        didSet { UserDefaults.standard.set(unlockCharacters, forKey: "opens4ka.unlockCharacters") }
    }
    @Published var ultrawide: Bool {
        didSet { UserDefaults.standard.set(ultrawide, forKey: "opens4ka.ultrawide") }
    }
    @Published var aspectRatio: UInt32 {
        didSet { UserDefaults.standard.set(Int(aspectRatio), forKey: "opens4ka.aspectRatio") }
    }
    @Published var screenFilter: UInt32 {
        didSet { UserDefaults.standard.set(Int(screenFilter), forKey: "opens4ka.screenFilter") }
    }
    @Published var antiAliasing: UInt32 {
        didSet { UserDefaults.standard.set(Int(antiAliasing), forKey: "opens4ka.antiAliasing") }
    }
    /// action id to key names, the runtime's format; every action has an entry
    @Published private(set) var keyBindings: [String: String] {
        didSet { UserDefaults.standard.set(keyBindings, forKey: "opens4ka.keys") }
    }
    @Published var playstationPrompts: Bool {
        didSet { UserDefaults.standard.set(playstationPrompts, forKey: "opens4ka.playstationPrompts") }
    }
    private var gameRunning = false
    private var pollTimer: Timer?
    private var inputTimer: Timer?
    private var heldButtons = Set<String>()

    init() {
        let defaults = UserDefaults.standard
        unlockCharacters = defaults.object(forKey: "opens4ka.unlockCharacters") == nil
            ? false
            : defaults.bool(forKey: "opens4ka.unlockCharacters")
        ultrawide = defaults.object(forKey: "opens4ka.ultrawide") == nil
            ? true
            : defaults.bool(forKey: "opens4ka.ultrawide")
        aspectRatio = defaults.object(forKey: "opens4ka.aspectRatio") == nil
            ? 178
            : UInt32(clamping: defaults.integer(forKey: "opens4ka.aspectRatio"))
        screenFilter = defaults.object(forKey: "opens4ka.screenFilter") == nil
            ? 0
            : UInt32(clamping: defaults.integer(forKey: "opens4ka.screenFilter"))
        antiAliasing = defaults.object(forKey: "opens4ka.antiAliasing") == nil
            ? 0
            : UInt32(clamping: defaults.integer(forKey: "opens4ka.antiAliasing"))
        keyBindings = KeyBindings.defaults.merging(
            defaults.dictionary(forKey: "opens4ka.keys") as? [String: String] ?? [:]) { _, saved in saved }
        playstationPrompts = defaults.object(forKey: "opens4ka.playstationPrompts") == nil
            ? true
            : defaults.bool(forKey: "opens4ka.playstationPrompts")
        GCController.shouldMonitorBackgroundEvents = true
        refreshController()
        let timer = Timer(timeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.poll() }
        }
        pollTimer = timer
        RunLoop.main.add(timer, forMode: .common)
        let input = Timer(timeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.pollLauncherInput() }
        }
        inputTimer = input
        RunLoop.main.add(input, forMode: .common)
    }

    var isAvailable: Bool {
        opens4ka_runtime_available() != 0
    }

    func isAvailable(for runtimeName: String) -> Bool {
        runtimeName.withCString { opens4ka_runtime_available_for_game($0) != 0 }
    }

    var controllerSummary: String {
        controllerConnected ? "\(controllerKind) · \(controllerName)" : "NO CONTROLLER CONNECTED"
    }

    var aspectRatioLabel: String {
        switch aspectRatio {
        case 133: return "4:3"
        case 160: return "16:10"
        case 178: return "16:9 WIDE"
        case 200: return "18:9"
        default: return aspectRatio == 0 ? "FULL SCREEN" : "CUSTOM"
        }
    }

    var screenFilterLabel: String {
        switch screenFilter {
        case 1: return "SHARPEN"
        case 2: return "VIVID"
        case 3: return "SCANLINES"
        case 4: return "CRT"
        default: return "OFF"
        }
    }

    var antiAliasingLabel: String {
        switch antiAliasing {
        case 1: return "FXAA"
        case 2: return "SSAA 2X"
        default: return "OFF"
        }
    }

    func refresh() {
        refreshController()
        status = isAvailable ? "Xbox native runtime ready" : "Xbox native runtime not installed"
    }

    @discardableResult
    func play(gameDirectory: URL, gameTitle: String = "Game", runtimeName: String = "OpenS4kaXbox") -> Bool {
        let path = gameDirectory.path
        opens4ka_runtime_set_playstation_prompts(playstationPrompts ? 1 : 0)
        KeyBindings.argument(keyBindings).withCString { opens4ka_runtime_set_keys($0) }
        let started = path.withCString { pathPointer in
            runtimeName.withCString { runtimePointer in
                opens4ka_runtime_start_with_options_for_game(pathPointer, runtimePointer,
                                                              unlockCharacters ? 1 : 0, ultrawide ? 1 : 0,
                                                              aspectRatio, screenFilter, antiAliasing) != 0
            }
        }
        if started {
            gameRunning = true
            status = "\(gameTitle) is running"
            // The native runtime owns the gameplay window. Hide the launcher
            // so it cannot remain visually on top while the child activates.
            NSApp.hide(nil)
        } else {
            status = String(cString: opens4ka_runtime_last_error())
        }
        return started
    }

    func poll() {
        refreshController()
        guard gameRunning else { return }
        if opens4ka_runtime_poll() != 0 { return }
        gameRunning = false
        NSApp.unhide(nil)
        NSApp.activate(ignoringOtherApps: true)
        let error = String(cString: opens4ka_runtime_last_error())
        status = error.isEmpty ? "Game stopped" : error
    }

    func stop() {
        opens4ka_runtime_stop()
        gameRunning = false
        NSApp.unhide(nil)
        NSApp.activate(ignoringOtherApps: true)
        refresh()
    }

    /// One key for the action; an action that had it gets this one's old keys, so nothing is left doubly bound.
    func bind(_ action: String, to key: String) {
        var map = keyBindings
        let previous = map[action] ?? ""
        for (other, keys) in map where other != action {
            let list = keys.split(separator: ",").map(String.init)
            if list.contains(key) {
                let rest = list.filter { $0 != key }
                map[other] = rest.isEmpty ? previous : rest.joined(separator: ",")
            }
        }
        map[action] = key
        keyBindings = map
    }

    func resetKeys() { keyBindings = KeyBindings.defaults }

    func openBluetoothSettings() {
        guard let url = URL(string: "x-apple.systempreferences:com.apple.BluetoothSettings") else { return }
        NSWorkspace.shared.open(url)
    }

    private func refreshController() {
        let controllers = GCController.controllers()
        guard let controller = controllers.first(where: isDualShock4) ?? controllers.first else {
            controllerConnected = false
            controllerName = "No controller connected"
            controllerKind = "CONTROLLER"
            return
        }

        controllerConnected = true
        controllerName = controller.vendorName?.isEmpty == false
            ? controller.vendorName!
            : "Wireless Controller"
        controllerKind = controller.extendedGamepad is GCDualSenseGamepad ? "DUALSENSE"
            : isDualShock4(controller) ? "DUALSHOCK 4" : "GAMEPAD"
    }

    private func isDualShock4(_ controller: GCController) -> Bool {
        let vendor = (controller.vendorName ?? "").lowercased()
        let category = controller.productCategory.lowercased()
        return controller.extendedGamepad is GCDualSenseGamepad || controller.extendedGamepad is GCDualShockGamepad
            || vendor.contains("dualshock") || vendor.contains("dualsense") || vendor.contains("wireless controller")
            || category.contains("dualshock") || category.contains("dualsense") || category.contains("ps4")
            || category.contains("ps5")
    }

    private func pollLauncherInput() {
        guard !gameRunning, NSApp.isActive,
              let controller = GCController.controllers().first(where: isDualShock4) ?? GCController.controllers().first,
              let pad = controller.extendedGamepad else {
            heldButtons.removeAll()
            return
        }
        let bindings: [(String, Bool, LauncherAction)] = [
            ("a", pad.buttonA.isPressed, .play),
            ("b", pad.buttonB.isPressed, .back),
            ("x", pad.buttonX.isPressed || pad.buttonMenu.isPressed, .settings),
            ("l1", pad.leftShoulder.isPressed, .games),
            ("r1", pad.rightShoulder.isPressed, .settings),
            ("up", pad.dpad.up.isPressed || pad.leftThumbstick.yAxis.value > 0.6, .up),
            ("down", pad.dpad.down.isPressed || pad.leftThumbstick.yAxis.value < -0.6, .down),
            ("left", pad.dpad.left.isPressed || pad.leftThumbstick.xAxis.value < -0.6, .left),
            ("right", pad.dpad.right.isPressed || pad.leftThumbstick.xAxis.value > 0.6, .right)
        ]
        let pressed = Set(bindings.filter { $0.1 }.map { $0.0 })
        let actions = bindings.filter { $0.1 && !heldButtons.contains($0.0) }.map { $0.2 }
        heldButtons = pressed
        for action in actions { launcherActions.send(action) }
    }
}
