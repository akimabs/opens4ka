#pragma once
// Patches over guest textures, matched by the texture's contents: the PlayStation button prompts drawn over the
// Xbox ones in a font's atlas. textures/<hash>.patch beside the executable, the hash 16 hex digits:
// u32 width, u32 height, u32 count, then per rect u16 x, y, w, h and w * h BGRA texels blended over level 0.
#include <cstdint>
#include <vector>

#include "gpu/texture.h"

namespace texpack {

struct Rect {
    uint16_t x, y, w, h;
    std::vector<uint8_t> bgra;
};
struct Patch {
    uint32_t w = 0, h = 0;
    std::vector<Rect> rects;
};

void Enable(bool on);  // before the first draw: off leaves the Xbox prompts
bool Active();         // any patch loaded, or dumping on
uint64_t ContentHash(const gpu::TextureDesc& d);
const Patch* Find(uint64_t hash, const gpu::TextureDesc& d);
void Apply(const Patch& p, std::vector<uint8_t>& bgra, uint32_t pitch);
// level l from the patched level 0 (w by h, packed), box filtered
void Level(const std::vector<uint8_t>& base, uint32_t w, uint32_t h, uint32_t level, std::vector<uint8_t>& out);
// OPENS4KA_TEX_DUMP=<dir>: each small texture's level 0 written there once as <hash>.tex: u32 w, u32 h, BGRA
void Dump(uint64_t hash, const gpu::TextureDesc& d, const std::vector<uint8_t>& bgra, uint32_t pitch);

}  // namespace texpack
