// Mortal Kombat: Shaolin Monks needs no patch of its code to run: the XDK it calls is all on the host. Its own
// hooks go here, [[hook]] entries in config/game.toml and X86_FUNC replacements of recompiled functions.
#include <cstring>

#include "common/aspect.h"
#include "common/options.h"
#include "cpu/x86_context.h"

namespace game {

// the disc has English, French, German, Spanish and Italian; Indonesian is the settings panel's alone
bool LanguageAvailable(const std::string& code) { return KnownLanguage(code); }

// no HUD hook to tell play from a menu: the touch controls stay up
bool HudShown() { return true; }

}  // namespace game

namespace app {

// the console's XC_LANGUAGE the game reads at boot, from the language setting as this run started
uint32_t ConsoleLanguage() {
    const std::string& l = game::BootLanguage();
    if (l == "deu") return 3;
    if (l == "fra") return 4;
    if (l == "esp") return 5;
    if (l == "ita") return 6;
    return 1;  // English, and Indonesian, which the disc lacks
}

}  // namespace app

// The game's cameras use a 4:3 projection even when the rendered frame is shown wider. Replace only the usual
// gameplay/camera aspect values, leaving render-to-texture cameras with their own shapes intact.
void WidenAspect(X86Context& ctx, uint8_t*) {
    if (!game::Ultrawide()) return;
    float aspect;
    std::memcpy(&aspect, &ctx.eax, 4);
    if (aspect < 1.15f || aspect > 1.4f) return;
    aspect = game::Aspect();
    std::memcpy(&ctx.eax, &aspect, 4);
}
