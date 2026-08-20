#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace wolf {
namespace ntfs {

struct MftDirEntry {
    uint64_t parentMft = 0;
    std::string name;
    bool inUse = false;
    bool isDirectory = false;
    int pathConfidence = 50;
};

// Parent/name index built from $MFT FILE records and INDX slack hints.
class MftIndex {
public:
    static constexpr uint64_t kRootMft = 5;

    void putFileRecord(uint64_t mftRecord, uint64_t parentMft, const std::string& name,
                       bool inUse, bool isDirectory);
    void putIndxHint(uint64_t mftRecord, uint64_t parentMft, const std::string& name);
    void putLogNameHint(const std::string& name);

    std::string rebuildPath(uint64_t mftRecord, const std::string& fileName,
                            uint64_t parentMft) const;

    bool hasRecord(uint64_t mftRecord) const;

private:
    std::unordered_map<uint64_t, MftDirEntry> entries_;
    std::unordered_map<std::string, int> logDirHints_;

    const MftDirEntry* lookup(uint64_t mft) const;
};

} // namespace ntfs
} // namespace wolf
