#pragma once

namespace byteback {

// Assembled RAID is opt-in via driveIndex -1 / drivePath "raid" (hex, scan,
// recover, content-search). A live array must not hijack PhysicalDrive or
// volume I/O — that would silently mix member bytes with the virtual layout.
inline bool usesRaidBackend(bool raidActive, int driveIndex) {
    return raidActive && driveIndex == -1;
}

inline bool hexUsesRaidBackend(bool raidActive, int driveIndex) {
    return usesRaidBackend(raidActive, driveIndex);
}

} // namespace byteback
