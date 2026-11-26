#include "runtime/hires_textures.h"
#include "runtime/config.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb/stb_image.h"

namespace nwii {
namespace runtime {
namespace hle {
namespace {

struct PackIndex {
    bool ready = false;
    std::unordered_map<uint64_t, std::string> by_hash;
};

PackIndex& pack_index() {
    static PackIndex index;
    return index;
}

std::mutex& pack_mu() {
    static std::mutex mu;
    return mu;
}

bool is_hex16(const std::string& s) {
    if (s.size() != 16)
        return false;
    for (unsigned char c : s) {
        if (!std::isxdigit(c))
            return false;
    }
    return true;
}

uint64_t parse_hex16(const std::string& s) {
    return std::strtoull(s.c_str(), nullptr, 16);
}

// tex1_<w>x<h>_<hash>_<format>[_mipN].png
// The hash is the original texture. The PNG itself is often 4x that size.
bool parse_dolphin_name(const std::string& filename, uint64_t& hash, bool& mip) {
    std::string stem = filename;
    auto dot = stem.rfind('.');
    if (dot != std::string::npos)
        stem.resize(dot);
    if (stem.rfind("tex1_", 0) != 0)
        return false;

    mip = stem.find("_mip") != std::string::npos;
    std::string token;
    std::string hash_tok;
    for (size_t i = 0; i <= stem.size(); ++i) {
        if (i == stem.size() || stem[i] == '_') {
            if (is_hex16(token))
                hash_tok = token;
            if (token == "m")
                mip = true;
            token.clear();
        } else {
            token.push_back(stem[i]);
        }
    }
    if (hash_tok.empty())
        return false;
    hash = parse_hex16(hash_tok);
    return true;
}

void index_tree(const std::filesystem::path& root, PackIndex& index,
                 std::unordered_set<std::string>& seen_dirs) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(root, ec))
        return;
    fs::path canon = fs::weakly_canonical(root, ec);
    if (ec)
        canon = root;
    if (!seen_dirs.insert(canon.string()).second)
        return;

    int added = 0;
    const auto dir_opts = fs::directory_options::skip_permission_denied |
                          fs::directory_options::follow_directory_symlink;
    for (auto it = fs::recursive_directory_iterator(root, dir_opts, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file())
            continue;
        const std::string name = it->path().filename().string();
        uint64_t hash = 0;
        bool mip = false;
        if (!parse_dolphin_name(name, hash, mip))
            continue;
        auto found = index.by_hash.find(hash);
        if (found == index.by_hash.end()) {
            index.by_hash.emplace(hash, it->path().string());
            ++added;
        } else if (!mip) {
            // A base image replaces a mip that was indexed first.
            std::string existing = fs::path(found->second).filename().string();
            bool existing_mip = false;
            uint64_t ignored = 0;
            parse_dolphin_name(existing, ignored, existing_mip);
            if (existing_mip) {
                found->second = it->path().string();
                ++added;
            }
        }
    }
    std::cout << "[Hires] " << added << " textures from " << root.string()
              << std::endl;
}

void ensure_index() {
    PackIndex& index = pack_index();
    if (index.ready)
        return;
    std::lock_guard<std::mutex> lock(pack_mu());
    if (index.ready)
        return;

    const Config& cfg = Config::get();
    std::vector<std::filesystem::path> roots;
    if (!cfg.texture_pack.empty())
        roots.emplace_back(cfg.texture_pack);
    if (!cfg.game_dir.empty()) {
        namespace fs = std::filesystem;
        fs::path game(cfg.game_dir);
        const std::string& id = cfg.game_id;
        roots.push_back(game / "Load" / "Textures" / id);
        if (id.size() >= 3)
            roots.push_back(game / "Load" / "Textures" / id.substr(0, 3));
        roots.push_back(game / "SOM");
        if (game.has_parent_path())
            roots.push_back(game.parent_path() / "SOM");
    }

    std::unordered_set<std::string> seen;
    for (const auto& root : roots)
        index_tree(root, index, seen);

    std::cout << "[Hires] " << index.by_hash.size()
              << " custom textures indexed" << std::endl;
    index.ready = true;
}

} // namespace

uint64_t dolphin_texture_hash(const uint8_t* tex, size_t tex_size,
                              const uint8_t* tlut, size_t tlut_size) {
    XXH64_state_t* st = XXH64_createState();
    XXH64_reset(st, 0);
    if (tex && tex_size)
        XXH64_update(st, tex, tex_size);
    if (tlut && tlut_size)
        XXH64_update(st, tlut, tlut_size);
    uint64_t h = XXH64_digest(st);
    XXH64_freeState(st);
    return h;
}

bool load_hires_rgba(uint64_t hash, int& width, int& height, uint8_t*& rgba) {
    rgba = nullptr;
    width = 0;
    height = 0;
    ensure_index();

    auto it = pack_index().by_hash.find(hash);
    if (it == pack_index().by_hash.end())
        return false;

    std::error_code ec;
    const auto sz = std::filesystem::file_size(it->second, ec);
    if (ec || sz == 0 || sz > 64ull * 1024ull * 1024ull)
        return false;
    std::vector<unsigned char> file(static_cast<size_t>(sz));
    FILE* fp = std::fopen(it->second.c_str(), "rb");
    if (!fp)
        return false;
    size_t nread = std::fread(file.data(), 1, file.size(), fp);
    std::fclose(fp);
    if (nread != file.size())
        return false;

    int comp = 0;
    unsigned char* pixels = stbi_load_from_memory(
        file.data(), static_cast<int>(file.size()), &width, &height, &comp, 4);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels)
            stbi_image_free(pixels);
        return false;
    }
    size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    rgba = static_cast<uint8_t*>(std::malloc(bytes));
    if (!rgba) {
        stbi_image_free(pixels);
        return false;
    }
    std::memcpy(rgba, pixels, bytes);
    stbi_image_free(pixels);
    std::cout << "[Hires] " << std::hex << hash << std::dec << " -> "
              << width << "x" << height << " " << it->second << std::endl;
    return true;
}

} // namespace hle
} // namespace runtime
} // namespace nwii
