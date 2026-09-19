#pragma once

#include "fs/partition_scanner.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace byteback {

struct ResolvedVolume {
    int driveIndex = -1;
    int64_t partitionStartSector = -1;
    uint64_t partitionSizeSectors = 0;
    VolumeFsKind fsKind = VolumeFsKind::Unknown;
    uint32_t diskExtentCount = 1;
    // All PhysicalDrive indices from VOLUME_DISK_EXTENTS (unique, order preserved).
    std::vector<int> diskNumbers;
    // ASCII "\\.\X:" — scan binds this when diskExtentCount > 1.
    std::string volumePath;
};

// "\\.\C:" — only a drive-letter volume device, never PhysicalDrive or UNC.
inline bool isWin32VolumeDevicePath(const std::string& p) {
    if (p.size() != 6) return false;
    if (p[0] != '\\' || p[1] != '\\' || p[2] != '.' || p[3] != '\\') return false;
    const unsigned char letter = static_cast<unsigned char>(p[4]);
    if (!((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z'))) return false;
    return p[5] == ':';
}

/** Local evidence file — not a volume device, PhysicalDrive, http(s), or /dev/. */
inline bool isEvidenceImagePath(const std::string& p) {
    if (p.empty()) return false;
    if (p.size() >= 4 && p.compare(0, 4, "\\\\.\\") == 0) return false;
    auto startsNoCase = [](const std::string& s, const char* prefix) {
        for (size_t i = 0; prefix[i] != '\0'; ++i) {
            if (i >= s.size()) return false;
            char c = s[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != prefix[i]) return false;
        }
        return true;
    };
    if (startsNoCase(p, "http://") || startsNoCase(p, "https://")) return false;
    if (p.compare(0, 5, "/dev/") == 0) return false;
    return true;
}

// "d", "D:", "D:\\" -> L"D:"
std::optional<std::wstring> normalizeDriveLetter(const std::wstring& input);
std::optional<std::wstring> normalizeDriveLetterUtf8(const std::string& input);

const char* volumeFsKindLabel(VolumeFsKind kind);

#ifdef _WIN32
std::optional<ResolvedVolume> resolveDriveLetter(const std::wstring& letter);
std::vector<std::wstring> listLogicalDriveLetters();
#endif

} // namespace byteback
