#include "recovery/path_util.h"
#include <filesystem>

namespace wolf {

std::string safeBasename(const std::string& name) {
    std::filesystem::path p(name);
    std::string base = p.filename().string();
    if (base.empty() || base == "." || base == "..") base = "recovered_file.bin";
    for (char& c : base) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    return base;
}

std::string uniqueDestPath(const std::string& destDir, const std::string& name) {
    const std::string base = safeBasename(name);
    std::filesystem::path first = std::filesystem::path(destDir) / base;
    if (!std::filesystem::exists(first)) return first.string();

    std::filesystem::path stem = first.stem();
    std::filesystem::path ext = first.extension();
    for (int n = 1; n < 10000; ++n) {
        std::filesystem::path candidate = std::filesystem::path(destDir) /
            (stem.string() + "_" + std::to_string(n) + ext.string());
        if (!std::filesystem::exists(candidate)) return candidate.string();
    }
    return {};
}

bool destDirIsSafe(const std::string& destDir) {
    if (destDir.empty()) return false;
    return destDir.find("..") == std::string::npos;
}

} // namespace wolf
