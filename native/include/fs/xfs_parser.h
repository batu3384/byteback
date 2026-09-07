#pragma once

// Read-only XFS filesystem parser (Linux XFS, big-endian on-disk structures).
// Scope: superblock validation, deterministic inode location, extent/B+tree
// data fork decode, shortform/block/leaf/node directory walk (dir2+dir3/v5).
// Conservative: unknown magics/formats are reported, never guessed.

#include "byteback_io.h"
#include "fs/partition_scanner.h" // VolumeFsKind
#include <functional>
#include <string>
#include <vector>

namespace byteback {

struct XfsSuperblock {
    uint32_t blocksize = 0;
    uint64_t dblocks = 0;
    uint32_t agblocks = 0;
    uint32_t agcount = 0;
    uint64_t rootino = 0;
    uint8_t blocklog = 0;
    uint8_t inodelog = 0;
    uint8_t inopblog = 0;
    uint8_t agblklog = 0;
    uint16_t versionnum = 0;
    uint16_t inodesize = 0;
    // v3 inodes exist only on XFS_SB_VERSION_5 filesystems (versionnum numbits == 5).
    // The old body here tested 0x2000 calling it "the CRC bit", but 0x2000 is
    // XFS_SB_VERSION_DIRV2BIT (xfs_format.h) and is set on v4 filesystems too,
    // which would decode v2 inodes with the v3 fork offset (176 instead of 100).
    bool hasV3Inodes() const { return (versionnum & 0x000f) == 5; }
};

class XfsParser {
public:
    using FileCallback = std::function<void(const std::string& path, uint64_t inodeNo,
                                            uint64_t sizeBytes, bool isDirectory,
                                            const std::vector<std::pair<uint64_t, uint64_t>>& runs)>;

    bool open(DiskReader& reader, uint64_t partitionOffsetBytes);
    const XfsSuperblock& superblock() const { return sb_; }
    bool walkTree(const FileCallback& cb); // recursive from rootino

    // Single inode: returns false on unreadable/unsupported (honest skip).
    bool readInode(uint64_t inodeNo, std::vector<uint8_t>& out) const;

private:
    DiskReader* reader_ = nullptr;
    uint64_t partOffset_ = 0;
    XfsSuperblock sb_{};
};

} // namespace byteback
