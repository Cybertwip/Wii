#pragma once

#include "loader/loader.h"
#include <string>

namespace nwii {
namespace recomp {

// Resolved once per run. Empty install_dir / mcp_dir means
// ~/Tools/ghidra_11.3.2_PUBLIC and ~/Tools/GhidraMCP-release-1-4.
struct GhidraHookConfig {
    bool enabled = true;
    std::string install_dir;
    std::string mcp_dir;
    std::string output_dir;
    int analysis_timeout_sec = 180;
};

struct GhidraHookPaths {
    std::string install_dir;
    std::string mcp_dir;
    std::string analyze_headless;
    std::string mcp_bridge;
    std::string mcp_zip;
};

// Fills paths from cfg, then ~/Tools/..., and installs the GhidraMCP
// extension into that Ghidra if the release zip is present.
// Returns false when the Ghidra install cannot be found.
bool prepare_ghidra_hook(const GhidraHookConfig& cfg, GhidraHookPaths& out);

// Headless-import the DOL image and write a Name,Location,Size CSV.
// Reuses csv_path when it already contains symbols.
// Failure leaves the recompiler free to continue with its own analyzer.
bool export_symbols_with_ghidra(const GhidraHookConfig& cfg,
                                 const loader::Executable& exec,
                                 const std::string& csv_path);

} // namespace recomp
} // namespace nwii
