#pragma once

#include "fs/ntfs_util.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace byteback {
namespace ntfs {

struct RecycleIInfo {
    uint64_t fileSize = 0;
    int64_t deletedUnix = 0;
    std::string originalPath;
    bool ok = false;
};

inline uint64_t readU64le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

inline RecycleIInfo parseRecycleIFile(const uint8_t* data, size_t n) {
    RecycleIInfo out;
    if (!data || n < 26) return out;
    const uint64_t ver = readU64le(data);
    if (ver != 1 && ver != 2) return out;
    out.fileSize = readU64le(data + 8);
    out.deletedUnix = filetimeToUnix(readU64le(data + 16));
    size_t pathOff = 24;
    auto utf16LooksLikePath = [&](size_t off) {
        if (off + 4 > n) return false;
        const uint16_t c0 = static_cast<uint16_t>(data[off] | (data[off + 1] << 8));
        const uint16_t c1 = static_cast<uint16_t>(data[off + 2] | (data[off + 3] << 8));
        if (c0 == '\\' || c0 == '/') return true;
        if (c0 >= 'A' && c0 <= 'Z' && c1 == ':') return true;
        if (c0 >= 'a' && c0 <= 'z' && c1 == ':') return true;
        return false;
    };
    if (ver == 2 && !utf16LooksLikePath(pathOff) && utf16LooksLikePath(28)) pathOff = 28;
    if (pathOff >= n) return out;
    size_t units = (n - pathOff) / 2;
    const auto* u = reinterpret_cast<const uint16_t*>(data + pathOff);
    size_t len = 0;
    while (len < units && u[len] != 0) ++len;
    out.originalPath = utf16leToUtf8(u, len);
    out.ok = !out.originalPath.empty();
    return out;
}

inline bool recyclePairKey(const std::string& name, char& kind, std::string& id) {
    if (name.size() < 3 || name[0] != '$') return false;
    const char k = static_cast<char>(std::toupper(static_cast<unsigned char>(name[1])));
    if (k != 'I' && k != 'R') return false;
    kind = k;
    id = name.substr(2);
    for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return !id.empty();
}

inline std::string recycleFileName(const std::string& path) {
    std::string p = path;
    for (char& c : p) if (c == '\\') c = '/';
    const auto slash = p.find_last_of('/');
    if (slash == std::string::npos) return p;
    return p.substr(slash + 1);
}

// Pair $I metadata with $R content. $I rows become ntfs_recycle_meta.
template <typename Rec>
void applyRecycleBinRecords(std::vector<Rec>& files) {
    struct Pair { int iIdx = -1; int rIdx = -1; };
    std::unordered_map<std::string, Pair> pairs;
    for (int i = 0; i < static_cast<int>(files.size()); ++i) {
        char kind = 0;
        std::string id;
        if (!recyclePairKey(files[i].fr.name, kind, id)) continue;
        auto& p = pairs[id];
        if (kind == 'I') p.iIdx = i;
        else p.rIdx = i;
    }
    for (auto& kv : pairs) {
        const Pair& p = kv.second;
        if (p.iIdx < 0 || p.rIdx < 0) continue;
        Rec& iRec = files[static_cast<size_t>(p.iIdx)];
        Rec& rRec = files[static_cast<size_t>(p.rIdx)];
        RecycleIInfo info = parseRecycleIFile(
            iRec.fr.residentData.empty() ? nullptr : iRec.fr.residentData.data(),
            iRec.fr.residentData.size());
        if (!info.ok) continue;
        const std::string leaf = recycleFileName(info.originalPath);
        if (!leaf.empty()) rRec.fr.name = leaf;
        rRec.fr.path = info.originalPath;
        if (info.fileSize > 0) rRec.fr.sizeBytes = info.fileSize;
        if (info.deletedUnix > 0) rRec.fr.createdAt = info.deletedUnix;
        rRec.fr.source = "ntfs_recycle";
        rRec.fr.status = 0;
        rRec.fr.confidence = std::min(95, rRec.fr.confidence + 15);
        iRec.fr.source = "ntfs_recycle_meta";
    }
}

} // namespace ntfs
} // namespace byteback
