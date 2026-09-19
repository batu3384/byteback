#include "io/volume_mapper_win.h"
#include "byteback_io.h"
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iterator>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

namespace byteback {

namespace {

bool isDriveLetter(wchar_t c) {
    c = static_cast<wchar_t>(::towupper(c));
    return c >= L'A' && c <= L'Z';
}

} // namespace

std::optional<std::wstring> normalizeDriveLetter(const std::wstring& input) {
    if (input.empty()) return std::nullopt;
    wchar_t letter = 0;
    size_t i = 0;
    while (i < input.size() && ::iswspace(input[i])) ++i;
    if (i >= input.size() || !isDriveLetter(input[i])) return std::nullopt;
    letter = static_cast<wchar_t>(::towupper(input[i]));
    ++i;
    while (i < input.size() && ::iswspace(input[i])) ++i;
    if (i < input.size() && input[i] != L':') return std::nullopt;
    if (i < input.size()) {
        ++i;
        while (i < input.size() && (input[i] == L'\\' || input[i] == L'/')) ++i;
        while (i < input.size() && ::iswspace(input[i])) ++i;
        if (i < input.size()) return std::nullopt;
    }
    std::wstring out;
    out.push_back(letter);
    out.push_back(L':');
    return out;
}

std::optional<std::wstring> normalizeDriveLetterUtf8(const std::string& input) {
    if (input.empty()) return std::nullopt;
    std::wstring wide(input.begin(), input.end());
    return normalizeDriveLetter(wide);
}

const char* volumeFsKindLabel(VolumeFsKind kind) {
    switch (kind) {
        case VolumeFsKind::Ntfs: return "ntfs";
        case VolumeFsKind::ExFat: return "exfat";
        case VolumeFsKind::Fat: return "fat";
        case VolumeFsKind::Ext4: return "ext4";
        case VolumeFsKind::Apfs: return "apfs";
        case VolumeFsKind::Hfs: return "hfs";
        case VolumeFsKind::Refs: return "refs";
        case VolumeFsKind::Unread: return "unread";
        case VolumeFsKind::Lvm: return "lvm";
        case VolumeFsKind::Iso9660: return "iso9660";
        case VolumeFsKind::Udf: return "udf";
        case VolumeFsKind::Luks: return "luks";
        case VolumeFsKind::Spaces: return "spaces";
        default: return "unknown";
    }
}

#ifdef _WIN32

std::vector<std::wstring> listLogicalDriveLetters() {
    std::vector<std::wstring> out;
    wchar_t buf[512] = {};
    DWORD len = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(buf)), buf);
    if (len == 0 || len >= std::size(buf)) return out;
    for (wchar_t* p = buf; *p; p += wcslen(p) + 1) {
        auto norm = normalizeDriveLetter(p);
        if (norm) out.push_back(*norm);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::optional<ResolvedVolume> resolveDriveLetter(const std::wstring& letter) {
    auto norm = normalizeDriveLetter(letter);
    if (!norm || norm->size() < 2) return std::nullopt;

    wchar_t volPath[16];
    swprintf_s(volPath, L"\\\\.\\%lc:", (*norm)[0]);

    HANDLE hVol = CreateFileW(volPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) return std::nullopt;

    std::vector<uint8_t> buf(sizeof(VOLUME_DISK_EXTENTS) + 16 * sizeof(DISK_EXTENT));
    DWORD br = 0;
    auto* ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf.data());
    for (;;) {
        if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                            nullptr, 0, ext, static_cast<DWORD>(buf.size()), &br, nullptr)
            && ext->NumberOfDiskExtents > 0) {
            break;
        }
        if (GetLastError() != ERROR_MORE_DATA) {
            CloseHandle(hVol);
            return std::nullopt;
        }
        if (buf.size() > sizeof(VOLUME_DISK_EXTENTS) + 256 * sizeof(DISK_EXTENT)) {
            CloseHandle(hVol);
            return std::nullopt;
        }
        buf.resize(buf.size() + 16 * sizeof(DISK_EXTENT));
        ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf.data());
    }

    const uint32_t extentCount = ext->NumberOfDiskExtents;
    const DISK_EXTENT& e0 = ext->Extents[0];
    CloseHandle(hVol);

    ResolvedVolume rv;
    rv.driveIndex = static_cast<int>(e0.DiskNumber);
    rv.diskExtentCount = extentCount;
    rv.volumePath = std::string("\\\\.\\") + static_cast<char>((*norm)[0]) + ":";
    for (DWORD i = 0; i < extentCount; ++i) {
        const int n = static_cast<int>(ext->Extents[i].DiskNumber);
        if (n < 0) continue;
        if (std::find(rv.diskNumbers.begin(), rv.diskNumbers.end(), n) == rv.diskNumbers.end())
            rv.diskNumbers.push_back(n);
    }
    if (rv.driveIndex < 0) return std::nullopt;

    uint32_t sectorSize = 512;
    DiskReader reader;
    if (!reader.openDrive(rv.driveIndex)) return std::nullopt;
    sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    uint64_t offsetBytes = static_cast<uint64_t>(e0.StartingOffset.QuadPart);
    uint64_t lengthBytes = static_cast<uint64_t>(e0.ExtentLength.QuadPart);
    if (lengthBytes == 0) return std::nullopt;

    rv.partitionStartSector = static_cast<int64_t>(offsetBytes / sectorSize);
    rv.partitionSizeSectors = lengthBytes / sectorSize;
    if (rv.partitionSizeSectors == 0) return std::nullopt;

    rv.fsKind = probeVolumeAt(reader, offsetBytes, sectorSize);
    return rv;
}

#endif // _WIN32

} // namespace byteback
