#include "fs/ntfs_path_rebuild.h"
#include <algorithm>

namespace byteback {
namespace ntfs {

void MftIndex::putFileRecord(uint64_t mftRecord, uint64_t parentMft, const std::string& name,
                             bool inUse, bool isDirectory) {
    if (mftRecord == 0 || name.empty()) return;
    MftDirEntry e;
    e.parentMft = parentMft;
    e.name = name;
    e.inUse = inUse;
    e.isDirectory = isDirectory;
    e.pathConfidence = inUse ? 85 : 70;
    auto it = entries_.find(mftRecord);
    if (it == entries_.end() || e.pathConfidence >= it->second.pathConfidence) {
        entries_[mftRecord] = std::move(e);
    }
}

void MftIndex::putIndxHint(uint64_t mftRecord, uint64_t parentMft, const std::string& name) {
    if (mftRecord == 0 || name.empty()) return;
    auto it = entries_.find(mftRecord);
    if (it != entries_.end() && it->second.pathConfidence >= 60) return;
    MftDirEntry e;
    e.parentMft = parentMft;
    e.name = name;
    e.pathConfidence = 55;
    entries_[mftRecord] = std::move(e);
}

void MftIndex::putLogNameHint(const std::string& name) {
    if (name.empty() || name.size() > 240) return;
    logDirHints_[name] = 40;
}

const MftDirEntry* MftIndex::lookup(uint64_t mft) const {
    auto it = entries_.find(mft);
    return it == entries_.end() ? nullptr : &it->second;
}

bool MftIndex::hasRecord(uint64_t mftRecord) const {
    return entries_.find(mftRecord) != entries_.end();
}

std::string MftIndex::rebuildPath(uint64_t /*mftRecord*/, const std::string& fileName,
                                  uint64_t parentMft) const {
    if (fileName.empty()) return "/";
    std::string path = fileName;
    uint64_t curr = parentMft;
    int depth = 0;
    int minConfidence = 100;

    while (curr != 0 && curr != kRootMft && depth < 64) {
        const MftDirEntry* ent = lookup(curr);
        if (!ent || ent->name.empty()) break;
        path = ent->name + "/" + path;
        minConfidence = std::min(minConfidence, ent->pathConfidence);
        curr = ent->parentMft;
        ++depth;
    }

    if (depth == 0 && !logDirHints_.empty()) {
        size_t slash = fileName.find('/');
        if (slash != std::string::npos) {
            std::string maybeDir = fileName.substr(0, slash);
            if (logDirHints_.count(maybeDir)) minConfidence = std::min(minConfidence, 40);
        }
    }

    (void)minConfidence;
    if (path.empty() || path[0] != '/') return "/" + path;
    return path;
}

} // namespace ntfs
} // namespace byteback
