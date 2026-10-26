#include "texture_pack.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "core/log.h"
#include "gpu/shaders.h"
#include "kernel/kernel.h"

namespace texpack {

namespace {

constexpr uint32_t kMaxSide = 1024;

std::once_flag g_once;
bool g_enabled = true;
std::unordered_map<uint64_t, Patch> g_pack;
std::filesystem::path g_dump;
std::unordered_set<uint64_t> g_dumped;

template <typename T>
bool Get(std::ifstream& f, T& v) {
    return bool(f.read(reinterpret_cast<char*>(&v), sizeof v));
}

bool ReadPatch(const std::filesystem::path& file, Patch& p) {
    std::ifstream f(file, std::ios::binary);
    uint32_t count = 0;
    if (!Get(f, p.w) || !Get(f, p.h) || !Get(f, count) || p.w > kMaxSide || p.h > kMaxSide || count > 256) return false;
    for (uint32_t i = 0; i < count; i++) {
        Rect r;
        if (!Get(f, r.x) || !Get(f, r.y) || !Get(f, r.w) || !Get(f, r.h)) return false;
        if (uint32_t(r.x) + r.w > p.w || uint32_t(r.y) + r.h > p.h) return false;
        r.bgra.resize(size_t(r.w) * r.h * 4);
        if (!f.read(reinterpret_cast<char*>(r.bgra.data()), std::streamsize(r.bgra.size()))) return false;
        p.rects.push_back(std::move(r));
    }
    return true;
}

void Load() {
    if (const char* d = std::getenv("OPENS4KA_TEX_DUMP")) {
        g_dump = d;
        std::error_code ec;
        std::filesystem::create_directories(g_dump, ec);
    }
    std::error_code ec;
    if (!g_enabled) return;
    for (const auto& e : std::filesystem::directory_iterator(app::ExeDir() / "textures", ec)) {
        if (e.path().extension() != ".patch") continue;
        uint64_t hash = std::strtoull(e.path().stem().string().c_str(), nullptr, 16);
        Patch p;
        if (hash && ReadPatch(e.path(), p)) g_pack[hash] = std::move(p);
        else LOGW("texpack: %s unreadable", e.path().filename().string().c_str());
    }
    if (!g_pack.empty()) LOGI("texpack: %zu texture patches", g_pack.size());
}

}  // namespace

void Enable(bool on) { g_enabled = on; }

bool Active() {
    std::call_once(g_once, Load);
    return !g_pack.empty() || !g_dump.empty();
}

// the level 0 texels as the guest stores them, with the shape: the same image anywhere in memory hashes the same
uint64_t ContentHash(const gpu::TextureDesc& d) {
    if (d.cube || d.depth > 1 || d.width > kMaxSide || d.height > kMaxSide) return 0;
    uint32_t shape[3] = {d.format, d.width, d.height};
    return gpu::Hash(kernel::HostPtr(d.data), gpu::LevelSize(d, 0), gpu::Hash(shape, sizeof shape));
}

const Patch* Find(uint64_t hash, const gpu::TextureDesc& d) {
    if (!hash || g_pack.empty()) return nullptr;
    auto it = g_pack.find(hash);
    return it != g_pack.end() && it->second.w == d.width && it->second.h == d.height ? &it->second : nullptr;
}

void Apply(const Patch& p, std::vector<uint8_t>& bgra, uint32_t pitch) {
    for (const Rect& r : p.rects)
        for (uint32_t y = 0; y < r.h; y++)
            for (uint32_t x = 0; x < r.w; x++) {
                size_t at = size_t(r.y + y) * pitch + size_t(r.x + x) * 4;
                if (at + 4 > bgra.size()) continue;
                const uint8_t* s = &r.bgra[(size_t(y) * r.w + x) * 4];
                uint8_t* t = &bgra[at];
                // the icon replaces what was there: its own alpha, the old icon's gone with it
                std::copy(s, s + 4, t);
            }
}

void Level(const std::vector<uint8_t>& base, uint32_t w, uint32_t h, uint32_t level, std::vector<uint8_t>& out) {
    uint32_t lw = std::max(w >> level, 1u), lh = std::max(h >> level, 1u);
    uint32_t sx = w / lw, sy = h / lh;
    out.assign(size_t(lw) * lh * 4, 0);
    for (uint32_t y = 0; y < lh; y++)
        for (uint32_t x = 0; x < lw; x++)
            for (int c = 0; c < 4; c++) {
                uint32_t sum = 0;
                for (uint32_t j = 0; j < sy; j++)
                    for (uint32_t i = 0; i < sx; i++) sum += base[(size_t(y * sy + j) * w + x * sx + i) * 4 + c];
                out[(size_t(y) * lw + x) * 4 + c] = uint8_t(sum / (sx * sy));
            }
}

void Dump(uint64_t hash, const gpu::TextureDesc& d, const std::vector<uint8_t>& bgra, uint32_t pitch) {
    if (g_dump.empty() || !hash || d.width > 1024 || d.height > 1024 || pitch < d.width * 4) return;
    if (!g_dumped.insert(hash).second) return;
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.tex", static_cast<unsigned long long>(hash));
    std::ofstream f(g_dump / name, std::ios::binary);
    f.write(reinterpret_cast<const char*>(&d.width), 4);
    f.write(reinterpret_cast<const char*>(&d.height), 4);
    for (uint32_t y = 0; y < d.height && size_t(y + 1) * pitch <= bgra.size(); y++)
        f.write(reinterpret_cast<const char*>(&bgra[size_t(y) * pitch]), std::streamsize(d.width) * 4);
}

}  // namespace texpack
