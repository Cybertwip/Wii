#include "runtime/devices.h"
#include "runtime/config.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <cstdio>

namespace nwii::runtime::devices {
namespace fs = std::filesystem;

static std::vector<uint8_t> make_sysconf() {
    std::vector<uint8_t> sc(0x4000, 0xFF);
    sc[0]='S'; sc[1]='C'; sc[2]='v'; sc[3]='1'; sc[4]='5';
    sc[5] = 1;
    sc[6] = 0; sc[7] = 1;
    uint8_t key[] = "IPL.AR";
    uint8_t val[] = { 0, 4, 'N','T','S','C' };
    int off = 8;
    sc[off++] = 0x02;
    sc[off++] = (uint8_t)(sizeof(key)-1);
    for (auto c : key) if(c) sc[off++] = c;
    for (auto b : val) sc[off++] = b;
    return sc;
}

static std::vector<uint8_t> make_setting_txt() {
    const char* txt =
        "AREA=USA\r\n"
        "MODEL=RVL-001\r\n"
        "DVD=0\r\n"
        "MPCH=0x7FFE\r\n"
        "CODE=0\r\n"
        "SERNO=123456789\r\n"
        "VIDEO=NTSC\r\n"
        "GAME=US\r\n";
    return std::vector<uint8_t>(txt, txt + strlen(txt));
}

static constexpr int32_t FS_INVALID = -101;
static constexpr int32_t FS_ACCESS = -102;
static constexpr int32_t FS_IO_ERROR = -103;
static constexpr int32_t FS_EXISTS = -105;
static constexpr int32_t FS_TOO_MANY_OPEN = -107;
static constexpr int32_t FS_NO_SPACE = -108;
static constexpr int32_t FS_NOT_EMPTY = -109;
static constexpr uint32_t BLOCK_SIZE = 0x4000;
static constexpr uint32_t MAX_BLOCKS = 0x8000;
static constexpr int FD_BASE = 20;

static bool valid_buffer(CPUContext& ctx, uint32_t addr, uint32_t len) {
    if (!len) return true;
    if (!addr || len > 0x4000000 || addr + len - 1 < addr) return false;
    auto* first = ctx.mmu.get_ptr(addr);
    auto* last = ctx.mmu.get_ptr(addr + len - 1);
    return first && last && reinterpret_cast<uintptr_t>(last) - reinterpret_cast<uintptr_t>(first) == len - 1;
}

static std::string read_path(CPUContext& ctx, uint32_t addr, uint32_t len) {
    len = std::min(len, 64u);
    if (!valid_buffer(ctx, addr, len)) return {};
    std::string result;
    for (uint32_t i = 0; i < len; ++i) {
        char c = static_cast<char>(ctx.mmu.read8(addr + i));
        if (!c) return result;
        result += c;
    }
    return {};
}

struct Attributes {
    uint32_t owner = 0;
    uint16_t group = 0;
    uint8_t owner_mode = 3, group_mode = 3, other_mode = 3, attr = 0;
};

struct Handle {
    fs::path path;
    uint32_t pos = 0;
    uint32_t mode = 0;
};

class FSDevice : public IDevice {
    fs::path root;
    std::array<Handle, 16> handles;
    std::map<fs::path, Attributes> attributes;

    fs::path host_path(const std::string& path) const {
        if (path.empty() || path.front() != '/' || path.size() >= 64 || path.find('\\') != std::string::npos)
            return {};
        fs::path result = root;
        for (const auto& part : fs::path(path).relative_path()) {
            if (part == ".." || part == ".") return {};
            result /= part;
            if (fs::is_symlink(fs::symlink_status(result))) return {};
        }
        return result;
    }

    bool initialize() {
        if (!root.empty()) return true;
        const auto& id = Config::get().game_id;
        if (id.empty() || id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
            return false;
        fs::path base;
        if (const char* override_dir = std::getenv("NWII_NAND_DIR")) base = override_dir;
#ifdef __APPLE__
        else if (const char* home = std::getenv("HOME")) base = fs::path(home) / "Library/Application Support/NWiiRecomp" / id;
#elif defined(_WIN32)
        else if (const char* appdata = std::getenv("LOCALAPPDATA")) base = fs::path(appdata) / "NWiiRecomp" / id;
#else
        else if (const char* xdg = std::getenv("XDG_DATA_HOME")) base = fs::path(xdg) / "NWiiRecomp" / id;
        else if (const char* home = std::getenv("HOME")) base = fs::path(home) / ".local/share/NWiiRecomp" / id;
#endif
        if (base.empty()) return false;
        fs::path candidate = base / "nand";
        uint32_t low = 0;
        for (size_t i = 0; i < 4 && i < id.size(); ++i)
            low |= static_cast<uint32_t>(static_cast<uint8_t>(id[i])) << ((3 - i) * 8);
        char title[32];
        std::snprintf(title, sizeof(title), "title/00010000/%08x", low);
        try {
            // IOS clears its temporary NAND directory at boot.
            fs::remove_all(candidate / "tmp");
            for (const char* dir : {"tmp", "import", "meta", "ticket", "title", "shared1", "shared2/sys", "shared2/test2", "sys", "title/00000001/00000002/data"})
                fs::create_directories(candidate / dir);
            fs::create_directories(candidate / title / "data");
            fs::create_directories(candidate / title / "content");
            auto seed = [&](const char* path, const std::vector<uint8_t>& data) {
                auto file = candidate / path;
                if (fs::exists(file)) return;
                std::ofstream stream(file, std::ios::binary);
                stream.write(reinterpret_cast<const char*>(data.data()), data.size());
                if (!stream) throw std::runtime_error("cannot seed NAND file");
            };
            seed("shared2/sys/SYSCONF", make_sysconf());
            seed("title/00000001/00000002/data/setting.txt", make_setting_txt());
            seed("title/00000001/00000002/data/play_rec.dat", std::vector<uint8_t>(0x20));
            seed("shared2/test2/dvderror.dat", std::vector<uint8_t>(4));
        } catch (const std::exception& e) {
            std::cerr << "[FS] NAND initialization failed: " << e.what() << '\n';
            return false;
        }
        root = fs::absolute(candidate);
        std::cout << "[FS] NAND: " << root << '\n';
        return true;
    }

    Handle* handle(uint32_t fd) {
        if (fd < FD_BASE || fd - FD_BASE >= handles.size()) return nullptr;
        auto& h = handles[fd - FD_BASE];
        return h.path.empty() ? nullptr : &h;
    }

    static Attributes read_attributes(CPUContext& ctx, uint32_t addr) {
        return {ctx.mmu.read32(addr), ctx.mmu.read16(addr + 4), ctx.mmu.read8(addr + 70),
                ctx.mmu.read8(addr + 71), ctx.mmu.read8(addr + 72), ctx.mmu.read8(addr + 73)};
    }

    static std::pair<uint32_t, uint32_t> usage(const fs::path& path) {
        uint32_t blocks = 0, inodes = 1;
        if (fs::is_regular_file(path)) return {static_cast<uint32_t>((fs::file_size(path) + BLOCK_SIZE - 1) / BLOCK_SIZE), 1};
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            if (entry.is_symlink()) continue;
            ++inodes;
            if (entry.is_regular_file()) blocks += (entry.file_size() + BLOCK_SIZE - 1) / BLOCK_SIZE;
        }
        return {blocks, inodes};
    }

public:
    const char* get_name() const override { return "/dev/fs"; }
    bool matches_path(const std::string& path) const override {
        return path == "/dev/fs" || (path.starts_with('/') && !path.starts_with("/dev/"));
    }

    int32_t open(CPUContext&, const std::string& path, uint32_t mode) override {
        if (!initialize()) return FS_IO_ERROR;
        if (path == "/dev/fs") return IPC_OK;
        try {
            auto file = host_path(path);
            if (file.empty() || mode < 1 || mode > 3) return FS_INVALID;
            if (!fs::is_regular_file(file)) {
                std::cout << "[FS] open: no such file " << path << '\n';
                return IPC_ENOENT;
            }
            for (size_t i = 0; i < handles.size(); ++i) {
                if (handles[i].path.empty()) {
                    handles[i] = {file, 0, mode};
                    std::cout << "[FS] open " << path << " mode=" << mode << '\n';
                    return FD_BASE + i;
                }
            }
            return FS_TOO_MANY_OPEN;
        } catch (const fs::filesystem_error&) { return FS_IO_ERROR; }
    }

    int32_t close(CPUContext&, uint32_t fd) override {
        if (!fd) return IPC_OK;
        auto* h = handle(fd);
        if (!h) return FS_INVALID;
        *h = {};
        return IPC_OK;
    }

    int32_t read(CPUContext& ctx, uint32_t fd, uint32_t buf, uint32_t len) override {
        auto* h = handle(fd);
        if (!h || !valid_buffer(ctx, buf, len)) return FS_INVALID;
        if (!(h->mode & 1)) return FS_ACCESS;
        if (!len) return 0;
        std::ifstream stream(h->path, std::ios::binary);
        if (!stream) return FS_IO_ERROR;
        stream.seekg(h->pos);
        stream.read(reinterpret_cast<char*>(ctx.mmu.get_ptr(buf)), len);
        if (stream.bad()) return FS_IO_ERROR;
        const auto count = stream.gcount();
        h->pos += count;
        return count;
    }

    int32_t write(CPUContext& ctx, uint32_t fd, uint32_t buf, uint32_t len) override {
        auto* h = handle(fd);
        if (!h || !valid_buffer(ctx, buf, len)) return FS_INVALID;
        if (!(h->mode & 2)) return FS_ACCESS;
        if (static_cast<uint64_t>(h->pos) + len > static_cast<uint64_t>(MAX_BLOCKS) * BLOCK_SIZE) return FS_NO_SPACE;
        if (!len) return 0;
        std::fstream stream(h->path, std::ios::binary | std::ios::in | std::ios::out);
        if (!stream) return FS_IO_ERROR;
        stream.seekp(h->pos);
        stream.write(reinterpret_cast<const char*>(ctx.mmu.get_ptr(buf)), len);
        stream.flush();
        if (!stream) return FS_IO_ERROR;
        h->pos += len;
        std::cout << "[FS] write " << h->path.filename() << " bytes=" << len << '\n';
        return len;
    }

    int32_t seek(CPUContext&, uint32_t fd, int32_t offset, int32_t whence) override {
        auto* h = handle(fd);
        if (!h || whence < 0 || whence > 2) return FS_INVALID;
        try {
            int64_t size = fs::file_size(h->path);
            int64_t pos = (whence == 0 ? 0 : whence == 1 ? h->pos : size) + offset;
            if (pos < 0 || pos > size) return FS_INVALID;
            h->pos = static_cast<uint32_t>(pos);
            return h->pos;
        } catch (const fs::filesystem_error&) { return FS_IO_ERROR; }
    }

    int32_t ioctl(CPUContext& ctx, const IpcRequest& req) override {
        if (!initialize()) return FS_IO_ERROR;
        if (!valid_buffer(ctx, req.in_buf, req.in_size) || !valid_buffer(ctx, req.out_buf, req.out_size)) return FS_INVALID;
        try {
            if (req.ioctl_cmd == 0xB) {
                auto* h = handle(req.fd);
                if (!h || req.out_size < 8) return FS_INVALID;
                ctx.mmu.write32(req.out_buf, fs::file_size(h->path));
                ctx.mmu.write32(req.out_buf + 4, h->pos);
                return IPC_OK;
            }
            if (req.ioctl_cmd == 2) {
                if (req.out_size < 28) return FS_INVALID;
                auto [blocks, inodes] = usage(root);
                const uint32_t stats[] = {BLOCK_SIZE, MAX_BLOCKS - std::min(MAX_BLOCKS, blocks), blocks, 0, 0, inodes, 0x1800 - std::min(0x1800u, inodes)};
                for (unsigned i = 0; i < 7; ++i) ctx.mmu.write32(req.out_buf + i * 4, stats[i]);
                return IPC_OK;
            }
            const bool attr_input = req.ioctl_cmd == 3 || req.ioctl_cmd == 5 || req.ioctl_cmd == 9;
            if (req.in_size < (attr_input ? 76u : 64u)) return FS_INVALID;
            const auto path = read_path(ctx, req.in_buf + (attr_input ? 6 : 0), 64);
            auto file = host_path(path);
            std::cout << "[FS] ioctl " << req.ioctl_cmd << " " << path << '\n';
            if (file.empty()) return FS_INVALID;
            if (req.ioctl_cmd == 3 || req.ioctl_cmd == 9) {
                if (fs::exists(file)) return FS_EXISTS;
                if (!fs::is_directory(file.parent_path())) return IPC_ENOENT;
                if (req.ioctl_cmd == 3) fs::create_directory(file);
                else {
                    std::ofstream stream(file, std::ios::binary);
                    if (!stream) return FS_IO_ERROR;
                }
                attributes[file] = read_attributes(ctx, req.in_buf);
                return IPC_OK;
            }
            if (!fs::exists(file)) return IPC_ENOENT;
            switch (req.ioctl_cmd) {
            case 5:
                attributes[file] = read_attributes(ctx, req.in_buf);
                return IPC_OK;
            case 6: {
                if (req.out_size < 76) return FS_INVALID;
                const auto a = attributes[file];
                auto* out = ctx.mmu.get_ptr(req.out_buf);
                std::memset(out, 0, 76);
                std::memcpy(out + 6, path.c_str(), path.size() + 1);
                ctx.mmu.write32(req.out_buf, a.owner);
                ctx.mmu.write16(req.out_buf + 4, a.group);
                out[70] = a.owner_mode; out[71] = a.group_mode;
                out[72] = a.other_mode; out[73] = a.attr;
                return IPC_OK;
            }
            case 7:
                if (file == root) return FS_ACCESS;
                for (const auto& h : handles) if (h.path == file) return FS_ACCESS;
                if (fs::is_directory(file) && !fs::is_empty(file)) return FS_NOT_EMPTY;
                fs::remove(file);
                attributes.erase(file);
                return IPC_OK;
            case 8: {
                if (req.in_size < 128 || file == root) return FS_INVALID;
                auto target = host_path(read_path(ctx, req.in_buf + 64, 64));
                if (target.empty() || target == root) return FS_INVALID;
                if (!fs::is_directory(target.parent_path())) return IPC_ENOENT;
                fs::rename(file, target);
                for (auto& h : handles) if (h.path == file) h.path = target;
                if (auto node = attributes.extract(file); !node.empty()) {
                    attributes.erase(target);
                    node.key() = target;
                    attributes.insert(std::move(node));
                }
                return IPC_OK;
            }
            default: return FS_INVALID;
            }
        } catch (const fs::filesystem_error& e) {
            std::cerr << "[FS] " << e.what() << '\n';
            return FS_IO_ERROR;
        }
    }

    int32_t ioctlv(CPUContext& ctx, const IpcRequest& req) override {
        if (!initialize()) return FS_IO_ERROR;
        const auto& v = req.ioctlv_vecs;
        if (v.size() != req.arg_cnt_in + req.arg_cnt_out || v.empty()) return FS_INVALID;
        for (const auto& vec : v) if (!valid_buffer(ctx, vec.addr, vec.len)) return FS_INVALID;
        try {
            const auto path = read_path(ctx, v[0].addr, v[0].len);
            auto file = host_path(path);
            std::cout << "[FS] ioctlv " << req.ioctl_cmd << " " << path << '\n';
            if (file.empty()) return FS_INVALID;
            if (!fs::exists(file)) return IPC_ENOENT;
            if (req.ioctl_cmd == 12 && req.arg_cnt_in == 1 && req.arg_cnt_out == 2 && v[1].len >= 4 && v[2].len >= 4) {
                auto [blocks, inodes] = usage(file);
                ctx.mmu.write32(v[1].addr, blocks);
                ctx.mmu.write32(v[2].addr, inodes);
                return IPC_OK;
            }
            if (req.ioctl_cmd == 4 && fs::is_directory(file)) {
                std::vector<std::string> names;
                for (const auto& entry : fs::directory_iterator(file))
                    if (!entry.is_symlink()) names.push_back(entry.path().filename().string());
                std::sort(names.begin(), names.end());
                if (req.arg_cnt_in == 1 && req.arg_cnt_out == 1 && v[1].len >= 4) {
                    ctx.mmu.write32(v[1].addr, names.size());
                    return IPC_OK;
                }
                if (req.arg_cnt_in == 2 && req.arg_cnt_out == 2 && v[1].len >= 4 && v[3].len >= 4) {
                    uint32_t max = ctx.mmu.read32(v[1].addr), count = 0, offset = 0;
                    for (const auto& name : names) {
                        if (count == max || name.size() + 1 > v[2].len - offset) break;
                        std::memcpy(ctx.mmu.get_ptr(v[2].addr + offset), name.c_str(), name.size() + 1);
                        offset += name.size() + 1;
                        ++count;
                    }
                    ctx.mmu.write32(v[3].addr, count);
                    return IPC_OK;
                }
            }
            return FS_INVALID;
        } catch (const fs::filesystem_error& e) {
            std::cerr << "[FS] " << e.what() << '\n';
            return FS_IO_ERROR;
        }
    }
};

std::unique_ptr<IDevice> create_fs_device() {
    return std::make_unique<FSDevice>();
}

}
