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
    std::string fname;
    // v3 inodes exist only on XFS_SB_VERSION_5 filesystems (versionnum numbits == 5).
    // The old body here tested 0x2000 calling it "the CRC bit", but 0x2000 is
    // XFS_SB_VERSION_DIRV2BIT (xfs_format.h) and is set on v4 filesystems too,
    // which would decode v2 inodes with the v3 fork offset (176 instead of 100).
    bool hasV3Inodes() const { return (versionnum & 0x000f) == 5; }
};

// Discovery sentinel path when a directory data block could not be read.
// Zero-fill is not an empty directory.
inline constexpr const char* kXfsDirUnreadPath = "/xfs-dir-unread/";
inline constexpr const char* kXfsSbUnreadPath = "/xfs-sb-unread/";
inline constexpr const char* kXfsSbUnreadSource = "xfs_sb_unread";
inline constexpr const char* kXfsInodeUnreadPath = "/xfs-inode-unread/";
inline constexpr const char* kXfsInodeUnreadSource = "xfs_inode_unread";
inline constexpr const char* kXfsBmapUnreadPath = "/xfs-bmap-unread/";
inline constexpr const char* kXfsBmapUnreadSource = "xfs_bmap_unread";
inline constexpr const char* kXfsUnlinkedPathPrefix = "/xfs-unlinked/";
inline constexpr const char* kXfsUnlinkedSource = "xfs_unlinked";
inline constexpr const char* kXfsVolNamePathPrefix = "/xfs-vol-name/";
inline constexpr const char* kXfsVolNameSource = "xfs_vol_name";

// xfs_dinode.di_crc is LE32 at offset 100 (XFS_DINODE_CRC_OFF). v1/v2 have no CRC.
bool xfsDinodeCrcOk(const uint8_t* inode, size_t len);

class XfsParser {
public:
    using FileCallback = std::function<void(const std::string& path, uint64_t inodeNo,
                                            uint64_t sizeBytes, bool isDirectory,
                                            const std::vector<std::pair<uint64_t, uint64_t>>& runs,
                                            int confidence, int64_t modifiedAt)>;

    bool open(DiskReader& reader, uint64_t partitionOffsetBytes);
    const XfsSuperblock& superblock() const { return sb_; }
    bool walkTree(const FileCallback& cb); // recursive from rootino

    // Single inode: returns false on unreadable/unsupported (honest skip).
    bool readInode(uint64_t inodeNo, std::vector<uint8_t>& out) const;

private:
    DiskReader* reader_ = nullptr;
    uint64_t partOffset_ = 0;
    XfsSuperblock sb_{};
    bool secondaryUnread_ = false;
};

} // namespace byteback
