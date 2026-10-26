#include <toml++/toml.hpp>

#include "app/app.h"
#include "audio/output.h"
#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"

namespace game {

namespace {

void Pad(int port, input::Pad& pad) {
    input::SeenPad(pad);
    if (port == 0) pad.connected = true;
    if (app::MenuOpen()) pad = input::Pad{.connected = pad.connected};
    else if (port == 0) input::Keyboard(pad);
}

void Rumble(int port, uint16_t& left, uint16_t& right) {
    if (port != 0 || !input::KeyboardActive()) return;
    left = 0;
    right = 0;
}

void Load(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    Options& o = Opt();
    o.mouse_sensitivity = float(t["mouse_sensitivity"].value_or(double(o.mouse_sensitivity)));
    o.touch_sensitivity = float(t["touch_sensitivity"].value_or(double(o.mouse_sensitivity)));
    std::string language = t["language"].value_or(o.language);
    if (KnownLanguage(language)) o.language = language;
    if (const toml::table* keys = t["keys"].as_table())
        for (auto&& [id, value] : *keys)
            if (auto v = value.value<std::string>()) o.keys[std::string(id.str())] = *v;
}

void Init() {
    BootLanguage();
    input::LoadBindings();
    audio::SetOutputMode(Opt().audio_output);
}

void Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    const Options& o = Opt();
    t.insert("mouse_sensitivity", double(o.mouse_sensitivity));
    t.insert("touch_sensitivity", double(o.touch_sensitivity));
    t.insert("language", o.language);
    toml::table keys;
    for (const auto& [id, value] : o.keys) keys.insert(id, value);
    t.insert("keys", keys);
}

}  // namespace

Options& Opt() {
    static Options o;
    return o;
}

const std::string& BootLanguage() {
    static const std::string language = Opt().language;
    return language;
}

bool LanguageAvailable(const std::string& code) { return KnownLanguage(code); }

// Digimon does not have a port-specific HUD hook yet; keeping controls visible
// is the same safe default used by the original runtime host.
bool HudShown() { return true; }

// No Mortal Kombat memory patches are applied to Digimon.
void ApplyCheats() {}

}  // namespace game

namespace app {

uint32_t ConsoleLanguage() {
    const std::string& l = game::BootLanguage();
    if (l == "deu") return 3;
    if (l == "fra") return 4;
    if (l == "esp") return 5;
    if (l == "ita") return 6;
    return 1;
}

const app::GameInfo kGame = {
    .title = L"Digimon Rumble Arena 2",
    .short_title = L"Digimon Rumble Arena 2",
    .name = L"DigimonRumbleArena2",
    .log = "digimonrumblearena2",
    .settings = "digimonrumblearena2.toml",
    .splash = nullptr,
    .missing = L"Put the Digimon Rumble Arena 2 ISO or default.xbe in the Digimon Rumble Arena 2 game folder.",
    .images = {{"default.xbe", "DigimonRumbleArena2"}},
    .options = {{"fps_counter", &game::Opt().fps_counter, nullptr},
                {"audio_output", nullptr, &game::Opt().audio_output},
                {"ultrawide", &game::Opt().ultrawide, nullptr},
                {"aspect_ratio", nullptr, &game::Opt().aspect_ratio},
                {"screen_filter", nullptr, &game::Opt().screen_filter},
                {"anti_aliasing", nullptr, &game::Opt().anti_aliasing}},
    .load = game::Load,
    .save = game::Save,
    .init = game::Init,
    .loaded = nullptr,
    .window = nullptr,
    .message = nullptr,
    .ui = app::DrawUi,
    .pad = game::Pad,
    .rumble = game::Rumble,
};

}  // namespace app
