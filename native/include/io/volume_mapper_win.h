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
};

// "d", "D:", "D:\\" -> L"D:"
std::optional<std::wstring> normalizeDriveLetter(const std::wstring& input);
std::optional<std::wstring> normalizeDriveLetterUtf8(const std::string& input);

const char* volumeFsKindLabel(VolumeFsKind kind);

#ifdef _WIN32
std::optional<ResolvedVolume> resolveDriveLetter(const std::wstring& letter);
std::vector<std::wstring> listLogicalDriveLetters();
#endif

} // namespace byteback
