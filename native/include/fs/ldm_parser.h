#pragma once

// Windows Logical Disk Manager (dynamic disks). PRIVHEAD is big-endian
// (linux block/partitions/ldm.c). Public region is the data volume.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace byteback {

class DiskReader;

struct LdmPrivhead {
    uint16_t verMajor = 0;
    uint16_t verMinor = 0;
    uint64_t logicalDiskStart = 0;
    uint64_t logicalDiskSize = 0;
    uint64_t configStart = 0;
    uint64_t configSize = 0;
    std::string diskGuid;
};

bool parseLdmPrivhead(const uint8_t* buf, size_t n, LdmPrivhead& out);
bool readLdmPrivhead(DiskReader& reader, LdmPrivhead& out);
bool looksLikeSpacedb(const uint8_t* buf, size_t n);

struct LdmPrt3 {
    uint64_t start = 0;
    uint64_t size = 0;
    uint64_t volumeOffset = 0;
    uint64_t parentId = 0;
    uint64_t diskId = 0;
};

bool parseLdmPrt3(const uint8_t* buf, size_t n, LdmPrt3& out);
bool readLdmPrt3(DiskReader& reader, const LdmPrivhead& ph, std::vector<LdmPrt3>& out);

struct LdmDsk3 {
    uint64_t objId = 0;
    std::string guid;
};

bool parseLdmDsk3(const uint8_t* buf, size_t n, LdmDsk3& out);
bool readLdmDsk3(DiskReader& reader, const LdmPrivhead& ph, std::vector<LdmDsk3>& out);

struct LdmCmp3 {
    uint64_t objId = 0;
    uint8_t type = 0;
    uint64_t children = 0;
    uint64_t parentId = 0;
    uint64_t chunkSectors = 0;
};

bool parseLdmCmp3(const uint8_t* buf, size_t n, LdmCmp3& out);
bool readLdmCmp3(DiskReader& reader, const LdmPrivhead& ph, std::vector<LdmCmp3>& out);

class VirtualRaid;
std::shared_ptr<VirtualRaid> assembleLdmFromOpenDisks(const std::vector<DiskReader*>& disks);
std::shared_ptr<VirtualRaid> assembleLdmFromEvidencePaths(const std::vector<std::string>& paths);

} // namespace byteback
