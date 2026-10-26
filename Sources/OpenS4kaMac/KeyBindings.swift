import AppKit

/// The keyboard's bindings for the game's controller, as the runtime names keys (runtime/host/touch.cpp kNames and
/// kActions). The launcher owns them and hands the whole map to each game it starts.
enum KeyBindings {
    struct Action: Identifiable {
        let id: String         // the runtime's action id
        let xbox: String       // the pad's name for it
        let playstation: String
        let defaults: String   // the runtime's default, comma separated
    }

    static let actions: [Action] = [
        Action(id: "move_up", xbox: "Move Up", playstation: "Move Up", defaults: "W"),
        Action(id: "move_down", xbox: "Move Down", playstation: "Move Down", defaults: "S"),
        Action(id: "move_left", xbox: "Move Left", playstation: "Move Left", defaults: "A"),
        Action(id: "move_right", xbox: "Move Right", playstation: "Move Right", defaults: "D"),
        Action(id: "walk", xbox: "Walk", playstation: "Walk", defaults: "Alt"),
        Action(id: "button_a", xbox: "A", playstation: "✕", defaults: "Space,K"),
        Action(id: "button_b", xbox: "B", playstation: "○", defaults: "L"),
        Action(id: "button_x", xbox: "X", playstation: "□", defaults: "J"),
        Action(id: "button_y", xbox: "Y", playstation: "△", defaults: "I"),
        Action(id: "button_white", xbox: "White", playstation: "L1", defaults: "U"),
        Action(id: "button_black", xbox: "Black", playstation: "R1", defaults: "O"),
        Action(id: "trigger_left", xbox: "LT", playstation: "L2", defaults: "Q"),
        Action(id: "trigger_right", xbox: "RT", playstation: "R2", defaults: "E"),
        Action(id: "thumb_left", xbox: "Click Left Stick", playstation: "L3", defaults: "C"),
        Action(id: "thumb_right", xbox: "Click Right Stick", playstation: "R3", defaults: "V"),
        Action(id: "dpad_up", xbox: "D-pad Up", playstation: "D-pad Up", defaults: "Up"),
        Action(id: "dpad_down", xbox: "D-pad Down", playstation: "D-pad Down", defaults: "Down"),
        Action(id: "dpad_left", xbox: "D-pad Left", playstation: "D-pad Left", defaults: "Left"),
        Action(id: "dpad_right", xbox: "D-pad Right", playstation: "D-pad Right", defaults: "Right"),
        Action(id: "start", xbox: "Start", playstation: "Start", defaults: "Enter,Escape"),
        Action(id: "back", xbox: "Back", playstation: "Select", defaults: "Backspace,Tab"),
        Action(id: "look_up", xbox: "Look Up", playstation: "Look Up", defaults: "Numpad8"),
        Action(id: "look_down", xbox: "Look Down", playstation: "Look Down", defaults: "Numpad5"),
        Action(id: "look_left", xbox: "Look Left", playstation: "Look Left", defaults: "Numpad4"),
        Action(id: "look_right", xbox: "Look Right", playstation: "Look Right", defaults: "Numpad6"),
    ]

    static var defaults: [String: String] {
        Dictionary(uniqueKeysWithValues: actions.map { ($0.id, $0.defaults) })
    }

    static func label(_ keys: String) -> String {
        keys.isEmpty ? "—" : keys.split(separator: ",").joined(separator: " / ")
    }

    /// "id=keys;id=keys", what --opens4ka-keys carries
    static func argument(_ map: [String: String]) -> String {
        actions.map { "\($0.id)=\(map[$0.id] ?? $0.defaults)" }.joined(separator: ";")
    }

    /// A key event's name for the runtime, by the key's position (US layout), nil for keys it cannot bind.
    static func name(for event: NSEvent) -> String? {
        if event.type == .flagsChanged {
            switch event.keyCode {
            case 56, 60: return event.modifierFlags.contains(.shift) ? "Shift" : nil
            case 59, 62: return event.modifierFlags.contains(.control) ? "Ctrl" : nil
            case 58, 61: return event.modifierFlags.contains(.option) ? "Alt" : nil
            case 57: return "CapsLock"
            default: return nil
            }
        }
        return keyNames[event.keyCode]
    }

    private static let keyNames: [UInt16: String] = [
        0: "A", 11: "B", 8: "C", 2: "D", 14: "E", 3: "F", 5: "G", 4: "H", 34: "I", 38: "J", 40: "K", 37: "L",
        46: "M", 45: "N", 31: "O", 35: "P", 12: "Q", 15: "R", 1: "S", 17: "T", 32: "U", 9: "V", 13: "W", 7: "X",
        16: "Y", 6: "Z",
        29: "0", 18: "1", 19: "2", 20: "3", 21: "4", 23: "5", 22: "6", 26: "7", 28: "8", 25: "9",
        49: "Space", 36: "Enter", 76: "Enter", 53: "Escape", 51: "Backspace", 48: "Tab",
        126: "Up", 125: "Down", 123: "Left", 124: "Right",
        114: "Insert", 117: "Delete", 115: "Home", 119: "End", 116: "PageUp", 121: "PageDown",
        122: "F1", 120: "F2", 118: "F4", 96: "F5", 97: "F6", 98: "F7", 100: "F8", 101: "F9",
        103: "F11", 111: "F12",
        82: "Numpad0", 83: "Numpad1", 84: "Numpad2", 85: "Numpad3", 86: "Numpad4", 87: "Numpad5",
        88: "Numpad6", 89: "Numpad7", 91: "Numpad8", 92: "Numpad9", 69: "NumpadPlus", 78: "NumpadMinus",
        67: "NumpadStar", 75: "NumpadSlash", 65: "NumpadPeriod",
        50: "Backtick", 27: "Minus", 24: "Equals", 33: "LBracket", 30: "RBracket", 42: "Backslash",
        41: "Semicolon", 39: "Quote", 43: "Comma", 47: "Period", 44: "Slash",
    ]
}
