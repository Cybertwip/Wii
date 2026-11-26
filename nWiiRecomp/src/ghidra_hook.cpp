#include "recompiler/ghidra_hook.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#ifndef NWII_GHIDRA_SCRIPT_DIR
#define NWII_GHIDRA_SCRIPT_DIR "nWiiAnalyzer/Ghidra"
#endif

namespace nwii {
namespace recomp {
namespace {

std::string home_dir() {
    if (const char* h = std::getenv("HOME")) {
        if (h[0] != '\0')
            return h;
    }
    return {};
}

bool dir_has_headless(const std::filesystem::path& root) {
    return std::filesystem::exists(root / "support" / "analyzeHeadless");
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

int count_csv_rows(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return 0;
    int rows = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty())
            ++rows;
    }
    return rows;
}

} // namespace

bool prepare_ghidra_hook(const GhidraHookConfig& cfg, GhidraHookPaths& out) {
    namespace fs = std::filesystem;
    const std::string home = home_dir();

    out.install_dir = cfg.install_dir;
    if (out.install_dir.empty() && !home.empty())
        out.install_dir = home + "/Tools/ghidra_11.3.2_PUBLIC";

    out.mcp_dir = cfg.mcp_dir;
    if (out.mcp_dir.empty() && !home.empty())
        out.mcp_dir = home + "/Tools/GhidraMCP-release-1-4";

    std::cout << "[Ghidra] install " << out.install_dir << "\n";
    std::cout << "[Ghidra] MCP     " << out.mcp_dir << "\n";

    if (!dir_has_headless(out.install_dir)) {
        std::cerr << "[Ghidra] analyzeHeadless not found under "
                  << out.install_dir << "\n";
        return false;
    }
    out.analyze_headless =
        (fs::path(out.install_dir) / "support" / "analyzeHeadless").string();

    fs::path bridge = fs::path(out.mcp_dir) / "bridge_mcp_ghidra.py";
    fs::path zip = fs::path(out.mcp_dir) / "GhidraMCP-1-4.zip";
    if (fs::exists(bridge))
        out.mcp_bridge = bridge.string();
    if (fs::exists(zip))
        out.mcp_zip = zip.string();

    // Ghidra refuses to start when the same module name exists in both the
    // application Extensions directory and the per-user extension directory.
    // Install into the user directory. Drop an application copy only when it
    // is the duplicate that causes that collision.
    const fs::path install_name = fs::path(out.install_dir).filename();
    fs::path user_ext = fs::path(home) / "Library" / "ghidra" / install_name /
                        "Extensions";
    fs::path user_jar = user_ext / "GhidraMCP" / "lib" / "GhidraMCP.jar";
    fs::path app_ext = fs::path(out.install_dir) / "Ghidra" / "Extensions" /
                       "GhidraMCP";
    fs::path app_jar = app_ext / "lib" / "GhidraMCP.jar";

    std::error_code ec;
    if (fs::exists(user_jar) && fs::exists(app_jar)) {
        fs::remove_all(app_ext, ec);
        if (ec)
            std::cerr << "[Ghidra] could not remove duplicate extension at "
                      << app_ext << " (" << ec.message() << ")\n";
        else
            std::cout << "[Ghidra] removed duplicate extension at " << app_ext
                      << "\n";
    }

    fs::path ext = fs::exists(user_jar) ? user_jar : app_jar;
    if (!fs::exists(ext) && !out.mcp_zip.empty()) {
        fs::path dest = home.empty() ? app_ext.parent_path() : user_ext;
        fs::create_directories(dest, ec);
        std::string cmd = "unzip -o -q " + shell_quote(out.mcp_zip) + " -d " +
                          shell_quote(dest.string());
        std::cout << "[Ghidra] installing GhidraMCP into " << dest << "\n";
        int rc = std::system(cmd.c_str());
        ext = dest / "GhidraMCP" / "lib" / "GhidraMCP.jar";
        if (rc != 0 || !fs::exists(ext)) {
            std::cerr << "[Ghidra] GhidraMCP install failed (status " << rc
                      << ")\n";
        } else {
            std::cout << "[Ghidra] GhidraMCP extension ready\n";
        }
    } else if (fs::exists(ext)) {
        std::cout << "[Ghidra] GhidraMCP extension already installed\n";
    } else {
        std::cerr << "[Ghidra] GhidraMCP zip not found at " << out.mcp_dir
                  << "\n";
    }

    if (!cfg.output_dir.empty()) {
        std::error_code ec;
        fs::create_directories(cfg.output_dir, ec);
        std::ofstream note(cfg.output_dir + "/ghidra_tools.toml");
        if (note) {
            note << "ghidra_dir = \"" << out.install_dir << "\"\n";
            note << "ghidra_mcp_dir = \"" << out.mcp_dir << "\"\n";
            note << "ghidra_mcp_bridge = \"" << out.mcp_bridge << "\"\n";
            note << "analyze_headless = \"" << out.analyze_headless << "\"\n";
        }
    }
    return true;
}

bool export_symbols_with_ghidra(const GhidraHookConfig& cfg,
                                 const loader::Executable& exec,
                                 const std::string& csv_path) {
    namespace fs = std::filesystem;
    GhidraHookPaths paths;
    if (!prepare_ghidra_hook(cfg, paths))
        return false;

    if (count_csv_rows(csv_path) > 1) {
        std::cout << "[Ghidra] reusing symbols " << csv_path << "\n";
        return true;
    }

    uint32_t lo = 0xFFFFFFFFu;
    uint32_t hi = 0;
    bool any = false;
    for (const auto& sec : exec.sections) {
        if (sec.is_bss || sec.data.empty() || sec.address < 0x80000000u)
            continue;
        uint32_t end = sec.address + static_cast<uint32_t>(sec.data.size());
        if (!any || sec.address < lo)
            lo = sec.address;
        if (end > hi)
            hi = end;
        any = true;
    }
    if (!any || hi <= lo) {
        std::cerr << "[Ghidra] DOL has no mapped sections\n";
        return false;
    }
    const uint64_t span = static_cast<uint64_t>(hi) - lo;
    if (span > 32ull * 1024ull * 1024ull) {
        std::cerr << "[Ghidra] image span 0x" << std::hex << span
                  << " is too large\n" << std::dec;
        return false;
    }
    if (exec.entry_point < lo || exec.entry_point >= hi) {
        std::cerr << "[Ghidra] entry 0x" << std::hex << exec.entry_point
                  << " is outside the image\n" << std::dec;
        return false;
    }

    std::vector<uint8_t> image(static_cast<size_t>(span), 0);
    for (const auto& sec : exec.sections) {
        if (sec.is_bss || sec.data.empty() || sec.address < lo)
            continue;
        size_t off = static_cast<size_t>(sec.address - lo);
        if (off >= image.size())
            continue;
        size_t n = sec.data.size();
        if (off + n > image.size())
            n = image.size() - off;
        std::memcpy(image.data() + off, sec.data.data(), n);
    }

    std::error_code ec;
    fs::create_directories(cfg.output_dir, ec);
    const fs::path out_dir(cfg.output_dir);
    const fs::path image_path = out_dir / "ghidra_mem.bin";
    const fs::path proj_dir = out_dir / "ghidra_project";
    const fs::path log_path = out_dir / "ghidra_headless.log";
    const fs::path script_path = out_dir / "run_ghidra_hook.sh";
    fs::create_directories(proj_dir, ec);

    {
        std::ofstream bin(image_path, std::ios::binary);
        if (!bin) {
            std::cerr << "[Ghidra] cannot write " << image_path << "\n";
            return false;
        }
        bin.write(reinterpret_cast<const char*>(image.data()),
                  static_cast<std::streamsize>(image.size()));
    }

    std::ostringstream entry_hex;
    entry_hex << std::hex << exec.entry_point;
    std::ostringstream base_hex;
    base_hex << "0x" << std::hex << lo;

    const std::string java21 =
        "/usr/local/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home";
    const int timeout = cfg.analysis_timeout_sec > 0 ? cfg.analysis_timeout_sec
                                                      : 180;

    std::ofstream sh(script_path);
    if (!sh) {
        std::cerr << "[Ghidra] cannot write " << script_path << "\n";
        return false;
    }
    sh << "#!/bin/bash\nset -u\n";
    if (fs::exists(java21))
        sh << "export JAVA_HOME=" << shell_quote(java21) << "\n";
    sh << "exec " << shell_quote(paths.analyze_headless) << " \\\n";
    sh << "  " << shell_quote(proj_dir.string()) << " NwiiHook \\\n";
    sh << "  -import " << shell_quote(image_path.string()) << " \\\n";
    sh << "  -loader BinaryLoader \\\n";
    sh << "  -loader-baseAddr " << base_hex.str() << " \\\n";
    sh << "  -processor PowerPC:BE:32:default \\\n";
    sh << "  -scriptPath " << shell_quote(NWII_GHIDRA_SCRIPT_DIR) << " \\\n";
    sh << "  -preScript MarkEntry.java " << entry_hex.str() << " \\\n";
    sh << "  -postScript ExportSymbolsHeadless.java "
       << shell_quote(csv_path) << " " << entry_hex.str() << " \\\n";
    sh << "  -analysisTimeoutPerFile " << timeout << " \\\n";
    sh << "  -deleteProject\n";
    sh.close();

    std::string cmd = "bash " + shell_quote(script_path.string()) + " > " +
                      shell_quote(log_path.string()) + " 2>&1";
    std::cout << "[Ghidra] headless analysis of " << image.size()
              << " bytes at " << base_hex.str() << " entry 0x" << entry_hex.str()
              << " (timeout " << timeout << "s)\n";
    int rc = std::system(cmd.c_str());
    int rows = count_csv_rows(csv_path);
    if (rc != 0 || rows <= 1) {
        std::cerr << "[Ghidra] symbol export failed (status " << rc
                  << ", rows " << rows << "). Log: " << log_path << "\n";
        return false;
    }
    std::cout << "[Ghidra] exported " << (rows - 1) << " symbols to "
              << csv_path << "\n";
    return true;
}

} // namespace recomp
} // namespace nwii
