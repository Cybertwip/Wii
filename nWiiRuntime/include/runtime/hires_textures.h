#pragma once

#include <cstddef>
#include <cstdint>

namespace nwii {
namespace runtime {
namespace hle {

// xxHash64 of the encoded GX bytes, then the TLUT bytes when the texture is
// paletted. Same stream Dolphin uses for tex1_<w>x<h>_<hash>_<fmt>.png.
uint64_t dolphin_texture_hash(const uint8_t* tex, size_t tex_size,
                              const uint8_t* tlut, size_t tlut_size);

// Loads the matching custom texture as tightly packed RGBA8.
// rgba is malloc'd; the caller frees it. Returns false on a miss.
bool load_hires_rgba(uint64_t hash, int& width, int& height, uint8_t*& rgba);

} // namespace hle
} // namespace runtime
} // namespace nwii
