#include "OpenS4kaRuntime.h"

#include <cerrno>
#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits.h>
#include <spawn.h>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern char** environ;

namespace {

namespace fs = std::filesystem;
pid_t g_child = -1;
std::string g_error;
bool g_playstation_prompts = true;
std::string g_keys;

fs::path RuntimeExecutable(const char* runtime_name) {
    const std::string runtime = runtime_name && *runtime_name ? runtime_name : "OpenS4kaXbox";
    if (runtime == "OpenS4kaXbox") {
        if (const char* configured = std::getenv("OPENS4KA_RUNTIME")) return configured;
    }
#if defined(__APPLE__)
    char path[PATH_MAX] = {};
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        fs::path executable(path);
        fs::path bundled_app = executable.parent_path().parent_path() /
                               ("Resources/Runtime/" + runtime + ".app/Contents/MacOS/" + runtime);
        if (fs::is_regular_file(bundled_app)) return bundled_app;
        fs::path bundled = executable.parent_path().parent_path() / ("Resources/Runtime/" + runtime);
        if (fs::is_regular_file(bundled)) return bundled;
    }
    #endif
    const char* home = std::getenv("HOME");
    return home ? fs::path(home) / ("Library/Application Support/OpenS4ka/Runtime/" + runtime)
                : fs::path(runtime);
}

void Error(std::string message) { g_error = std::move(message); }

struct RuntimeOptions {
    int unlock_characters = 0;
    int ultrawide = 0;
    uint32_t aspect_ratio = 0;
    uint32_t screen_filter = 0;
    uint32_t anti_aliasing = 0;
};

bool HasGameImage(const fs::path& directory) {
    if (fs::is_regular_file(directory / "default.xbe")) return true;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, ec)) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (entry.is_regular_file(ec) && (ext == ".iso" || ext == ".xiso")) return true;
    }
    return false;
}

}  // namespace

extern "C" const char* opens4ka_runtime_version(void) {
    return "macOS bridge 0.1";
}

extern "C" int opens4ka_runtime_available(void) {
    return fs::is_regular_file(RuntimeExecutable("OpenS4kaXbox")) ? 1 : 0;
}

extern "C" int opens4ka_runtime_available_for_game(const char* runtime_name) {
    return fs::is_regular_file(RuntimeExecutable(runtime_name)) ? 1 : 0;
}

int StartRuntime(const char* game_directory, const char* runtime_name, const RuntimeOptions* options) {
    if (g_child > 0) {
        Error("Game is already running.");
        return 0;
    }
    if (!game_directory || !*game_directory) {
        Error("A game folder has not been selected.");
        return 0;
    }

    fs::path game = game_directory;
    if (!fs::is_directory(game) || !HasGameImage(game)) {
        Error("No ISO or default.xbe was found in the game folder.");
        return 0;
    }

    fs::path executable = RuntimeExecutable(runtime_name);
    if (!fs::is_regular_file(executable)) {
        Error("The Xbox macOS runtime is not installed for this game.");
        return 0;
    }

    std::string executable_string = executable.string();
    std::string game_string = game.string();
    std::vector<std::string> arguments = {executable_string, game_string};
    if (options) {
        arguments.push_back("--opens4ka-unlock-characters");
        arguments.push_back(options->unlock_characters ? "1" : "0");
        arguments.push_back("--opens4ka-ultrawide");
        arguments.push_back(options->ultrawide ? "1" : "0");
        arguments.push_back("--opens4ka-aspect");
        arguments.push_back(std::to_string(options->aspect_ratio));
        arguments.push_back("--opens4ka-screen-filter");
        arguments.push_back(std::to_string(options->screen_filter));
        arguments.push_back("--opens4ka-anti-aliasing");
        arguments.push_back(std::to_string(options->anti_aliasing));
        arguments.push_back("--opens4ka-ps-prompts");
        arguments.push_back(g_playstation_prompts ? "1" : "0");
        if (!g_keys.empty()) {
            arguments.push_back("--opens4ka-keys");
            arguments.push_back(g_keys);
        }
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    pid_t child = -1;
    int result = posix_spawn(&child, executable_string.c_str(), nullptr, nullptr, argv.data(), environ);
    if (result != 0) {
        Error("Failed to start runtime: " + std::string(std::strerror(result)));
        return 0;
    }
    g_child = child;
    g_error.clear();
    return 1;
}

extern "C" void opens4ka_runtime_set_playstation_prompts(int on) { g_playstation_prompts = on != 0; }

extern "C" void opens4ka_runtime_set_keys(const char* keys) { g_keys = keys ? keys : ""; }

extern "C" int opens4ka_runtime_start(const char* game_directory) {
    return StartRuntime(game_directory, "OpenS4kaXbox", nullptr);
}

extern "C" int opens4ka_runtime_start_with_options(const char* game_directory, int unlock_characters, int ultrawide,
    uint32_t aspect_ratio, uint32_t screen_filter,
                                                     uint32_t anti_aliasing) {
    RuntimeOptions options{unlock_characters, ultrawide, aspect_ratio, screen_filter, anti_aliasing};
    return StartRuntime(game_directory, "OpenS4kaXbox", &options);
}

extern "C" int opens4ka_runtime_start_with_options_for_game(const char* game_directory, const char* runtime_name,
                                                               int unlock_characters, int ultrawide,
                                                               uint32_t aspect_ratio, uint32_t screen_filter,
                                                               uint32_t anti_aliasing) {
    RuntimeOptions options{unlock_characters, ultrawide, aspect_ratio, screen_filter, anti_aliasing};
    return StartRuntime(game_directory, runtime_name, &options);
}

extern "C" int opens4ka_runtime_poll(void) {
    if (g_child <= 0) return 0;

    int status = 0;
    pid_t result = waitpid(g_child, &status, WNOHANG);
    if (result == 0) return 1;
    if (result < 0) {
        Error("Runtime stopped: " + std::string(std::strerror(errno)));
        g_child = -1;
        return 0;
    }

    g_child = -1;
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code != 0) Error("Runtime stopped (exit code " + std::to_string(code) + "). Check Runtime/logs.");
    } else if (WIFSIGNALED(status)) {
        Error("Runtime crash (signal " + std::to_string(WTERMSIG(status)) + "). Cek Runtime/logs.");
    }
    return 0;
}

extern "C" void opens4ka_runtime_stop(void) {
    if (g_child <= 0) return;
    kill(g_child, SIGTERM);
    int status = 0;
    waitpid(g_child, &status, WNOHANG);
    g_child = -1;
}

extern "C" const char* opens4ka_runtime_last_error(void) {
    return g_error.c_str();
}
