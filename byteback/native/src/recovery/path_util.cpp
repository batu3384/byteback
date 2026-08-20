#include "recovery/path_util.h"
#include <filesystem>
#include <cctype>
#include <string>

namespace byteback {

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
    try {
        std::filesystem::path p(destDir);
        if (!p.is_absolute()) return false;
        for (const auto& part : p) {
            if (part == "..") return false;
        }
        std::string norm = p.lexically_normal().string();
        std::string lower;
        lower.reserve(norm.size());
        for (unsigned char c : norm) lower += static_cast<char>(std::tolower(c));
        if (lower.rfind("c:\\windows", 0) == 0) return false;
        if (lower.rfind("c:\\program files", 0) == 0) return false;
        if (lower.rfind("c:\\program files (x86)", 0) == 0) return false;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace byteback
