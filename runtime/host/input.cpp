// controllers through SDL on Android: as the SDK's runtime/core/input.cpp reads them, but the plug events come from
// the main thread's event loop (Android's input reaches SDL there), and port 0 also takes the touch controls
#include "core/input.h"

#ifdef __ANDROID__
#include <jni.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"
#include "host.h"

namespace input {

namespace {

// a controller takes the lowest free port when it is plugged in and keeps it until it is unplugged
std::mutex g_lock;
SDL_Gamepad* g_pads[4] = {};

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open", name ? name : "");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        LOGI("input: controller %d unplugged", i + 1);
    }
}

void Read(SDL_Gamepad* pad, Pad& p) {
    p.connected = true;
    constexpr SDL_GamepadButton kButtons[] = {
        SDL_GAMEPAD_BUTTON_DPAD_UP,    SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
    for (int i = 0; i < 8; i++) p.buttons |= uint16_t(SDL_GetGamepadButton(pad, kButtons[i]) << i);
    // A B X Y black white
    constexpr SDL_GamepadButton kAnalog[] = {SDL_GAMEPAD_BUTTON_SOUTH,          SDL_GAMEPAD_BUTTON_EAST,
                                             SDL_GAMEPAD_BUTTON_WEST,           SDL_GAMEPAD_BUTTON_NORTH,
                                             SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
    for (int i = 0; i < 6; i++) p.analog[i] = SDL_GetGamepadButton(pad, kAnalog[i]) ? 0xFF : 0;
    auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
    // 0 to 32767 into 0 to 255
    p.analog[6] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0) >> 7);
    p.analog[7] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0) >> 7);
    // SDL's sticks have y down; not plain negation: -32768 has no positive
    p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
    p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
    p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
    p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
}

// A controller Android hands over without its device: some phones' game services (MIUI's on a Redmi Note 8 Pro)
// take a DualShock 4 and pass its buttons and sticks on as a virtual device, which SDL takes for no controller.
// ShaolinMonksActivity sends those here as Android's key codes and axes; they play port 0.
#ifdef __ANDROID__
struct Loose {
    uint16_t buttons = 0;
    uint8_t analog[8] = {};
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0, hx = 0, hy = 0;
    bool seen = false;
};
std::mutex g_loose_lock;
Loose g_loose;

// Android's key code as the pad's: a button bit (dpad, start, back, thumbs) or an analog button, -1 for neither
void LooseKey(int code, int& bit, int& analog) {
    bit = analog = -1;
    switch (code) {
        case 19: bit = 0; break;    // DPAD_UP
        case 20: bit = 1; break;    // DPAD_DOWN
        case 21: bit = 2; break;    // DPAD_LEFT
        case 22: bit = 3; break;    // DPAD_RIGHT
        case 108: bit = 4; break;   // BUTTON_START (Options)
        case 109: bit = 5; break;   // BUTTON_SELECT (Share)
        case 106: bit = 6; break;   // BUTTON_THUMBL
        case 107: bit = 7; break;   // BUTTON_THUMBR
        case 96: analog = 0; break;   // BUTTON_A (cross)
        case 97: analog = 1; break;   // BUTTON_B (circle)
        case 99: analog = 2; break;   // BUTTON_X (square)
        case 100: analog = 3; break;  // BUTTON_Y (triangle)
        case 103: analog = 4; break;  // BUTTON_R1: black
        case 102: analog = 5; break;  // BUTTON_L1: white
        case 104: analog = 6; break;  // BUTTON_L2
        case 105: analog = 7; break;  // BUTTON_R2
        default: break;
    }
}

int16_t Axis(float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); }
uint8_t Trigger(float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f); }

// the loose controller's state on top of what SDL read: buttons added, the stick pushed further taken
void MergeLoose(Pad& p) {
    std::lock_guard lock(g_loose_lock);
    const Loose& l = g_loose;
    if (!l.seen) return;
    p.connected = true;
    p.buttons |= l.buttons;
    // the hat as the dpad: x left -1 right 1, y up -1 down 1
    if (l.hy < -0.5f) p.buttons |= 1 << 0;
    if (l.hy > 0.5f) p.buttons |= 1 << 1;
    if (l.hx < -0.5f) p.buttons |= 1 << 2;
    if (l.hx > 0.5f) p.buttons |= 1 << 3;
    for (int i = 0; i < 8; i++) p.analog[i] = std::max(p.analog[i], l.analog[i]);
    p.analog[6] = std::max(p.analog[6], Trigger(l.lt));
    p.analog[7] = std::max(p.analog[7], Trigger(l.rt));
    // Android's y is down, the pad's up
    auto further = [](int16_t& to, int16_t v) { if (std::abs(int(v)) > std::abs(int(to))) to = v; };
    further(p.lx, Axis(l.lx));
    further(p.ly, Axis(-l.ly));
    further(p.rx, Axis(l.rx));
    further(p.ry, Axis(-l.ry));
}

}  // namespace

// ShaolinMonksActivity: the loose controller's buttons, sticks and triggers, and its letting go when the app loses the focus
extern "C" JNIEXPORT void JNICALL Java_com_opens4ka_ShaolinMonksActivity_nativeLooseKey(JNIEnv*, jclass, jint code,
                                                                                     jboolean down) {
    int bit, analog;
    LooseKey(code, bit, analog);
    if (bit < 0 && analog < 0) return;
    std::lock_guard lock(g_loose_lock);
    g_loose.seen = true;
    if (bit >= 0) g_loose.buttons = uint16_t(down ? g_loose.buttons | (1u << bit) : g_loose.buttons & ~(1u << bit));
    if (analog >= 0) g_loose.analog[analog] = down ? 0xFF : 0;
}

extern "C" JNIEXPORT void JNICALL Java_com_opens4ka_ShaolinMonksActivity_nativeLooseAxes(JNIEnv*, jclass, jfloat lx,
                                                                                      jfloat ly, jfloat rx, jfloat ry,
                                                                                      jfloat lt, jfloat rt, jfloat hx,
                                                                                      jfloat hy) {
    std::lock_guard lock(g_loose_lock);
    g_loose.seen = true;
    g_loose.lx = lx, g_loose.ly = ly, g_loose.rx = rx, g_loose.ry = ry;
    g_loose.lt = lt, g_loose.rt = rt, g_loose.hx = hx, g_loose.hy = hy;
}

extern "C" JNIEXPORT void JNICALL Java_com_opens4ka_ShaolinMonksActivity_nativeLooseRelease(JNIEnv*, jclass) {
    std::lock_guard lock(g_loose_lock);
    bool seen = g_loose.seen;
    g_loose = {};
    g_loose.seen = seen;
}
#endif

// OPENS4KA_INPUT_SCRIPT="12:start,15:a,18:down" presses port 0's buttons that many seconds in, for unattended tests
struct Press {
    double at;
    int bit, analog;
    double hold = 0.2;
};

const std::vector<Press>& Script() {
    static const std::vector<Press> presses = [] {
        std::vector<Press> v;
        const char* s = std::getenv("OPENS4KA_INPUT_SCRIPT");
        if (!s) return v;
        std::string all(s);
        for (size_t at = 0; at < all.size();) {
            size_t end = all.find(',', at);
            if (end == std::string::npos) end = all.size();
            std::string item = all.substr(at, end - at);
            at = end + 1;
            size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            static const char* kBits[] = {"up", "down", "left", "right", "start", "back"};
            static const char* kAnalog[] = {"a", "b", "x", "y", "black", "white", "lt", "rt"};
            std::string name = item.substr(colon + 1);
            Press p{std::atof(item.c_str()), -1, -1};
            double hold = 0.2;  // "30:rt~10" holds ten seconds
            if (size_t t = name.find('~'); t != std::string::npos) {
                hold = std::atof(name.c_str() + t + 1);
                name.resize(t);
            }
            p.hold = hold;
            for (int i = 0; i < 8; i++) {
                if (i < 6 && name == kBits[i]) p.bit = i;
                if (name == kAnalog[i]) p.analog = i;
            }
            if (p.bit >= 0 || p.analog >= 0) v.push_back(p);
        }
        LOGI("input: %zu scripted presses", v.size());
        return v;
    }();
    return presses;
}

void MergeScript(Pad& p) {
    const std::vector<Press>& presses = Script();
    if (presses.empty()) return;
    static const auto start = std::chrono::steady_clock::now();
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    p.connected = true;
    for (const Press& x : presses) {
        if (now < x.at || now >= x.at + x.hold) continue;
        if (x.bit >= 0) p.buttons |= uint16_t(1u << x.bit);
        if (x.analog >= 0) p.analog[x.analog] = 0xFF;
    }
}

#ifndef __ANDROID__
}  // namespace
#endif

void Init() {
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

void OnPadEvent(const SDL_Event& e) {
    std::lock_guard lock(g_lock);
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
    else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
}

Pad Poll(int port) {
    Pad p;
    {
        std::lock_guard lock(g_lock);
        if (SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr) Read(pad, p);
    }
    // a controller pushed past a rest, not just plugged in
    constexpr int16_t kStickPushed = 16000;
    auto pushed = [](int16_t v) { return v > kStickPushed || v < -kStickPushed; };
#ifdef __ANDROID__
    if (port == 0) MergeLoose(p);
#endif
    if (port == 0) MergeScript(p);
    bool analog = std::any_of(std::begin(p.analog), std::end(p.analog), [](uint8_t a) { return a > 0x20; });
    if (p.buttons || analog || pushed(p.lx) || pushed(p.ly) || pushed(p.rx) || pushed(p.ry)) touch::PadUsed();
    touch::Apply(port, p);
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port >= 0 && port < 4 && g_pads[port]) SDL_RumbleGamepad(g_pads[port], left, right, 0);
}

}  // namespace input
