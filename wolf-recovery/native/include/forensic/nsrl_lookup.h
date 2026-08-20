#pragma once

#include <cstddef>
#include <string>

namespace forensic {

// On-disk SQLite MD5 set (NIST NSRL RDS scale). Text/CSV import builds
// `<path>.wolf-nsrl.sqlite` next to the source file.
class NsrlLookup {
public:
    NsrlLookup() = default;
    ~NsrlLookup();
    NsrlLookup(const NsrlLookup&) = delete;
    NsrlLookup& operator=(const NsrlLookup&) = delete;

    bool loadFromFile(const std::string& path);

    void clear();
    bool contains(const std::string& md5Hex) const;
    size_t size() const;
    const std::string& lastPath() const { return lastPath_; }

private:
    static bool normalizeMd5(std::string& hex);
    void closeDb();
    bool ensureDb(const std::string& sqlitePath);

    void* db_ = nullptr; // sqlite3*
    std::string lastPath_;
};

} // namespace forensic
