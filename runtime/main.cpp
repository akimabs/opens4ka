// Native macOS host for the recompiled Xbox title. The game, kernel and GPU code stay shared with Android;
// this host supplies the desktop window, settings, SDL event loop and local filesystem paths.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include <imgui.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "build_info.h"
#include "common/aspect.h"
#include "common/options.h"
#include "core/input.h"
#include "core/log.h"
#include "host.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "texture_pack.h"

namespace {

SDL_Window* g_window = nullptr;
#if defined(__APPLE__)
extern "C" void opens4ka_activate_macos_runtime(void);
#endif
std::filesystem::path g_exe_dir;
std::atomic<bool> g_foreground{true};
std::atomic<uint32_t> g_surface_epoch{0};
std::mutex g_foreground_lock;
std::condition_variable g_foreground_cv;

std::string Utf8(const wchar_t* w) {
    std::string s;
    for (; w && *w; ++w) s += *w < 0x80 ? char(*w) : '?';
    return s;
}

void SetForeground(bool on) {
    {
        std::lock_guard lock(g_foreground_lock);
        if (on && !g_foreground) ++g_surface_epoch;
        g_foreground = on;
    }
    g_foreground_cv.notify_all();
    LOGI("app: %s", on ? "in front" : "in the background");
}

app::Mode ScreenMode() {
    if (g_window) {
        int w = 0, h = 0;
        host::WindowPixels(w, h);
        if (w > 0 && h > 0) return {uint32_t(w), uint32_t(h)};
    }
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (!mode) return {1280, 720};
    return {uint32_t(mode->w * mode->pixel_density), uint32_t(mode->h * mode->pixel_density)};
}

float RefreshRate() {
    SDL_DisplayID display = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    return mode && mode->refresh_rate > 1 ? mode->refresh_rate : 60.0f;
}

uint32_t ParseOptionValue(const char* value, uint32_t fallback) {
    if (!value || !*value) return fallback;
    char* end = nullptr;
    unsigned long parsed = std::strtoul(value, &end, 10);
    return end && *end == '\0' ? uint32_t(parsed) : fallback;
}

void ApplyLauncherOptions(int argc, char** argv) {
    for (int i = 2; i + 1 < argc; i += 2) {
        const char* key = argv[i];
        const char* value = argv[i + 1];
        if (!std::strcmp(key, "--opens4ka-unlock-characters")) {
            game::Opt().unlock_characters = ParseOptionValue(value, game::Opt().unlock_characters ? 1 : 0) != 0;
        } else if (!std::strcmp(key, "--opens4ka-ultrawide")) {
            game::Opt().ultrawide = ParseOptionValue(value, game::Opt().ultrawide ? 1 : 0) != 0;
        } else if (!std::strcmp(key, "--opens4ka-aspect")) {
            game::Opt().aspect_ratio = ParseOptionValue(value, game::Opt().aspect_ratio);
        } else if (!std::strcmp(key, "--opens4ka-screen-filter")) {
            game::Opt().screen_filter = std::min(ParseOptionValue(value, game::Opt().screen_filter), 4u);
        } else if (!std::strcmp(key, "--opens4ka-keys")) {
            // the launcher's key bindings, "action=Key,Key;...": they replace the saved ones, LoadBindings reads them
            std::string_view all(value);
            while (!all.empty()) {
                std::string_view item = all.substr(0, all.find(';'));
                all.remove_prefix(std::min(all.size(), item.size() + 1));
                size_t eq = item.find('=');
                if (eq != std::string_view::npos && eq > 0)
                    game::Opt().keys[std::string(item.substr(0, eq))] = std::string(item.substr(eq + 1));
            }
        } else if (!std::strcmp(key, "--opens4ka-ps-prompts")) {
            texpack::Enable(ParseOptionValue(value, 1) != 0);
        } else if (!std::strcmp(key, "--opens4ka-anti-aliasing")) {
            game::Opt().anti_aliasing = std::min(ParseOptionValue(value, game::Opt().anti_aliasing), 2u);
        }
    }
}

}  // namespace

namespace host {

SDL_Window* Window() { return g_window; }
bool Foreground() {
#if defined(__APPLE__)
    // The launcher hands the screen to a nested runtime app. macOS can emit a
    // transient focus-lost event while that handoff is happening; treating it
    // as a background pause leaves the title permanently on a black frame.
    return true;
#else
    return g_foreground;
#endif
}
uint32_t SurfaceEpoch() { return g_surface_epoch; }

void WaitForeground() {
    std::unique_lock lock(g_foreground_lock);
    g_foreground_cv.wait(lock, [] { return g_foreground.load(); });
}

void WindowPixels(int& w, int& h) {
    w = h = 0;
    if (g_window) SDL_GetWindowSizeInPixels(g_window, &w, &h);
}

}  // namespace host

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};
constexpr int kDefaults = 5;

std::filesystem::path SettingsFile() { return ExeDir() / kGame.settings; }

void DesktopFrameDefaults(Settings& s) {
    s.display_mode = DisplayMode::Windowed;
    s.fps_cap = 60;
    s.vsync = true;
}

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"join_ip", s.join_ip},
                  {"defaults", int64_t(kDefaults)}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

Settings DefaultSettings() {
    Settings s;
    s.render_percent = 100;
    DesktopFrameDefaults(s);
    return s;
}

Settings& Get() {
    static Settings s = DefaultSettings();
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(SettingsFile().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; ++i)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    if (t["defaults"].value_or(int64_t(0)) < kDefaults) DesktopFrameDefaults(s);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

void Save() { std::ofstream(SettingsFile()) << Table() << "\n"; }

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s, build %s", kGame.title, IMAGE_XBE, date, BUILD_COMMIT);
    LOGR("Path: %s", ExeDir().c_str());
    LOGR("Platform: %s", SDL_GetPlatform());
    LOGR("CPU: %d threads", SDL_GetNumLogicalCPUCores());
    LOGR("RAM: %.1f GB", SDL_GetSystemRAM() / 1024.0);
    Mode desk = Desktop();
    LOGR("Display: %ux%u, %.0f Hz", desk.width, desk.height, RefreshRate());
    LogSettings();
}

Mode Desktop() { return ScreenMode(); }

Mode Output() {
    if (Get().width && Get().height) return {Get().width, Get().height};
    return Desktop();
}

bool Widescreen() { return game::Aspect() >= 1.5f; }

float RenderScale(uint32_t guest_height) {
    float h = Output().height * (Get().render_percent / 100.0f);
    if (game::Opt().anti_aliasing == game::kAaSsaa) h *= 2.0f;
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

std::filesystem::path ExeDir() {
    if (!g_exe_dir.empty()) return g_exe_dir;
    return std::filesystem::current_path();
}

int Instance() { return 0; }
void* Window() { return g_window; }
void ApplyDisplay() {}
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex lock;
    return lock;
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

namespace {

void InitUi() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendPlatformName = "sdl3";
}

void Dispatch(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_TERMINATING:
            logx::Flush();
            kernel::Shutdown(0);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            SetForeground(false);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            SetForeground(true);
            break;
        case SDL_EVENT_LOW_MEMORY:
            LOGW("app: macOS reported low memory");
            break;
        case SDL_EVENT_SENSOR_UPDATE:
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            gyro::OnEvent(e);
            return;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            input::OnPadEvent(e);
            break;
        default:
            break;
    }
    if (touch::OnEvent(e)) return;
    std::lock_guard lock(app::UiLock());
    ui::OnEvent(e);
}

}  // namespace

int main(int argc, char** argv) {
    std::error_code ec;
    g_exe_dir = std::filesystem::absolute(argc > 0 && argv[0] ? argv[0] : "OpenS4kaXbox", ec).parent_path();
    if (g_exe_dir.empty()) g_exe_dir = std::filesystem::current_path();
    if (argc > 1 && argv[1] && *argv[1]) ::setenv("OPENS4KA_GAME_DIR", argv[1], 1);
#if defined(__APPLE__)
    const std::filesystem::path bundled_icd = g_exe_dir.parent_path() / "vulkan/icd.d/MoltenVK_icd.json";
    const std::filesystem::path nested_bundled_icd =
        g_exe_dir.parent_path().parent_path().parent_path().parent_path() / "vulkan/icd.d/MoltenVK_icd.json";
    if (std::filesystem::is_regular_file(bundled_icd)) {
        ::setenv("VK_ICD_FILENAMES", bundled_icd.c_str(), 1);
    } else if (std::filesystem::is_regular_file(nested_bundled_icd)) {
        ::setenv("VK_ICD_FILENAMES", nested_bundled_icd.c_str(), 1);
    } else if (!std::getenv("VK_ICD_FILENAMES")) {
        ::setenv("VK_ICD_FILENAMES", "/opt/homebrew/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json", 1);
    }
#endif

    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    // OPENS4KA_QUIET: unattended test runs in a small window that never takes the focus or the screen
    const bool quiet = std::getenv("OPENS4KA_QUIET") != nullptr;
    if (quiet) SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
#if defined(__APPLE__)
    // The runtime is spawned by the Swift launcher instead of opened as a
    // standalone .app. Explicitly ask Cocoa/SDL to activate its window.
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "1");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO | SDL_INIT_SENSOR)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
    app::Load();
    ApplyLauncherOptions(argc, argv);
#if defined(__APPLE__)
    // The Swift launcher hands gameplay to this process. Start the native
    // runtime as a real fullscreen app so no intermediate desktop window is
    // visible during that handoff.
    app::Get().display_mode = app::DisplayMode::Fullscreen;
#endif
#if defined(__APPLE__)
    // macOS currently has no in-launcher settings panel. Skip the console
    // logos/movie on first launch so a successful boot reaches the menu.
    game::Opt().skip_intros = true;
    game::Opt().skip_cutscenes = true;
#endif
    if (quiet) app::Get().display_mode = app::DisplayMode::Windowed;
    if (app::kGame.init) app::kGame.init();
    app::LogSystem();

    uint32_t width = quiet ? 400 : app::Get().width ? app::Get().width : 1280;
    uint32_t height = quiet ? 300 : app::Get().height ? app::Get().height : 720;
    // Keep the drawable and the logical Cocoa window at the same size. On
    // macOS the high-density flag makes MoltenVK expose a 2x drawable, while
    // this nested runtime is handed a logical 1280x720 window by the launcher.
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    if (app::Get().display_mode == app::DisplayMode::Fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    else if (app::Get().display_mode == app::DisplayMode::Borderless) flags |= SDL_WINDOW_BORDERLESS;
    g_window = SDL_CreateWindow(Utf8(app::kGame.title).c_str(), int(width), int(height), flags);
    if (!g_window) {
        LOGE("app: no window: %s", SDL_GetError());
        return 1;
    }
    SDL_ShowWindow(g_window);
    if (quiet) {
        SDL_SetWindowPosition(g_window, 0, 0);
    } else {
#if defined(__APPLE__)
        opens4ka_activate_macos_runtime();
#endif
        SDL_RaiseWindow(g_window);
    }
    SDL_PumpEvents();
    LOGR("app: game window ready (%ux%u, mode %u)", width, height, uint32_t(app::Get().display_mode));

    app::GameFiles game;
    if (!app::FindGame(game)) {
        logx::Flush();
        return 1;
    }

    InitUi();
    input::Init();
    if (app::kGame.window) app::kGame.window(g_window);
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / "hdd").string())) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, Utf8(app::kGame.short_title).c_str(),
                                 "The game failed to start; see the log in the Runtime/logs folder.", g_window);
        return 1;
    }
    LOGR("app: game boot complete");

    // Boot can take long enough for Cocoa to put the launcher back on top.
    // Raise the actual game window once more after the runtime is ready.
    if (!quiet) {
        SDL_ShowWindow(g_window);
#if defined(__APPLE__)
        opens4ka_activate_macos_runtime();
#endif
        SDL_RaiseWindow(g_window);
    }
    SDL_PumpEvents();

    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) {
            Dispatch(e);
            while (SDL_PollEvent(&e)) Dispatch(e);
        }
        touch::Pump();
    }
}
