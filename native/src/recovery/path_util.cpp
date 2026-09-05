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

// Sanitized relative directory from an FS path ("/Users/x/Docs" or
// "C:\Users\x\Docs" or "Users/x/Docs") -> "Users/x/Docs". Drive prefixes,
// traversal segments and reserved device names are stripped; per-segment
// character sanitization matches safeBasename. Empty result means flat.
std::string safeRelativeDir(const std::string& fsPath) {
    if (fsPath.empty()) return {};
    std::string cleaned = fsPath;
    for (char& c : cleaned) {
        if (c == '\\') c = '/';
    }
    std::string out;
    size_t pos = 0;
    while (pos < cleaned.size()) {
        size_t slash = cleaned.find('/', pos);
        std::string seg = cleaned.substr(pos, (slash == std::string::npos ? cleaned.size() : slash) - pos);
        pos = (slash == std::string::npos) ? cleaned.size() : slash + 1;
        if (seg.empty() || seg == "." || seg == "..") continue;
        // Drive prefix ("C:") — keep the rest only.
        if (seg.size() == 2 && seg[1] == ':' && std::isalpha(static_cast<unsigned char>(seg[0]))) continue;
        for (char& c : seg) {
            if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
                c = '_';
            }
        }
        static const char* kReserved[] = {"CON",  "PRN",  "AUX",  "NUL",
                                          "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                          "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
        std::string upper = seg;
        for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        bool reserved = false;
        for (const char* r : kReserved) {
            if (upper == r) reserved = true;
        }
        if (reserved) seg += "_dir";
        if (!out.empty()) out += '/';
        out += seg;
    }
    return out;
}

std::string joinDestDir(const std::string& destDir, const std::string& relDir) {
    if (relDir.empty()) return destDir;
    return (std::filesystem::path(destDir) / std::filesystem::path(relDir)).lexically_normal().string();
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
