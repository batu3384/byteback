#pragma once

#include <cstddef>
#include <string>
#include <unordered_set>

namespace forensic {

// ponytail: in-memory MD5 set only — full NSRL (~100M+ rows) needs an on-disk
// bloom/SQLite index; upgrade path: mmap sorted hash file + binary search.
class NsrlLookup {
public:
    // Load MD5 hashes from a text file (32-char lowercase hex per line; # comments).
    // Returns false on I/O error; partial loads are kept.
    bool loadFromFile(const std::string& path);

    void clear();
    bool contains(const std::string& md5Hex) const;
    size_t size() const { return hashes_.size(); }
    const std::string& lastPath() const { return lastPath_; }

private:
    static bool normalizeMd5(std::string& hex);

    std::unordered_set<std::string> hashes_;
    std::string lastPath_;
};

} // namespace forensic
