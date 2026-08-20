#include "fs/vss_scanner.h"
#include "fs/partition_scanner.h"
#include "fs/volume_identity.h"
#include "wolf_fs.h"
#include <ctime>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace wolf {

namespace {

#ifdef _WIN32
uint64_t queryVolumeSizeBytes(HANDLE h) {
    DISK_GEOMETRY_EX geo{};
    DWORD br = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
            nullptr, 0, &geo, sizeof(geo), &br, nullptr)) {
        return geo.DiskSize.QuadPart;
    }
    GET_LENGTH_INFORMATION len{};
    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO,
            nullptr, 0, &len, sizeof(len), &br, nullptr)) {
        return len.Length.QuadPart;
    }
    return 0;
}
#endif

} // namespace

std::vector<VssSnapshotInfo> enumerateVssSnapshots() {
    std::vector<VssSnapshotInfo> out;
#ifdef _WIN32
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    for (int i = 1; i <= 64; ++i) {
        std::string path = "\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy" + std::to_string(i);
        std::wstring wpath(path.begin(), path.end());
        HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        VssSnapshotInfo info;
        info.index = i;
        info.devicePath = path;
        info.discoveredAt = now;
        info.sizeBytes = queryVolumeSizeBytes(h);
        FILETIME ftCreate{};
        if (GetFileTime(h, &ftCreate, nullptr, nullptr)) {
            ULARGE_INTEGER uli;
            uli.LowPart = ftCreate.dwLowDateTime;
            uli.HighPart = ftCreate.dwHighDateTime;
            constexpr uint64_t kEpochDiff = 116444736000000000ULL;
            if (uli.QuadPart > kEpochDiff)
                info.createdAt = static_cast<int64_t>((uli.QuadPart - kEpochDiff) / 10000000ULL);
        }
        CloseHandle(h);
        out.push_back(std::move(info));
    }
#endif
    return out;
}

void scanVssVolumeFilesystem(DiskReader& volumeReader, const VssSnapshotInfo& snap,
                             FileSystemParser::FileRecordCallback onFileFound,
                             VssScanProgressFn onProgress,
                             std::atomic<bool>* isRunning) {
    if (!volumeReader.isOpen()) return;

    uint32_t sectorSize = volumeReader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    const uint64_t totalSectors = volumeReader.getDiskSize() / sectorSize;
    const std::string vssPrefix = "/VSS" + std::to_string(snap.index);
    const int64_t snapTime = snap.createdAt > 0 ? snap.createdAt : snap.discoveredAt;

    auto tagAndEmit = [&](const FileRecord& fr, const char* source) {
        if (isRunning && !(*isRunning)) return;
        if (fr.id == -1 && fr.name.empty()) {
            onProgress(fr.startSector, totalSectors);
            return;
        }
        FileRecord tagged = fr;
        tagged.source = source;
        if (!tagged.path.empty() && tagged.path[0] == '/')
            tagged.path = vssPrefix + tagged.path;
        else
            tagged.path = vssPrefix + "/" + tagged.name;
        if (snapTime > 0) {
            if (tagged.createdAt == 0) tagged.createdAt = snapTime;
            if (tagged.modifiedAt == 0) tagged.modifiedAt = snapTime;
        }
        onFileFound(tagged);
        onProgress(tagged.startSector, totalSectors);
    };

    VolumeFsKind kind = probeVolumeAt(volumeReader, 0, sectorSize);
    switch (kind) {
        case VolumeFsKind::Ntfs: {
            NTFSParser ntfs;
            ntfs.scanAt(volumeReader, [&](const FileRecord& fr) {
                tagAndEmit(fr, "vss_ntfs");
            }, isRunning, 0, 0);
            break;
        }
        case VolumeFsKind::ExFat:
        case VolumeFsKind::Fat: {
            FATParser fat;
            fat.scanAt(volumeReader, [&](const FileRecord& fr) {
                tagAndEmit(fr, "vss_fat");
            }, isRunning, 0);
            break;
        }
        default:
            break;
    }
}

void scanVssSnapshots(FileSystemParser::FileRecordCallback onFileFound,
                      VssScanProgressFn onProgress,
                      std::atomic<bool>* isRunning) {
#ifdef _WIN32
    for (const auto& snap : enumerateVssSnapshots()) {
        if (isRunning && !(*isRunning)) break;

        FileRecord meta;
        meta.id = -1;
        meta.name = "VSS_Snapshot_" + std::to_string(snap.index);
        meta.path = snap.devicePath;
        meta.sizeBytes = snap.sizeBytes;
        meta.status = 1;
        meta.confidence = 95;
        meta.category = "System";
        meta.source = "vss_snapshot";
        meta.createdAt = snap.createdAt > 0 ? snap.createdAt : snap.discoveredAt;
        onFileFound(meta);

        DiskReader vssReader;
        if (!vssReader.openVolumePath(snap.devicePath)) continue;
        scanVssVolumeFilesystem(vssReader, snap, onFileFound, onProgress, isRunning);
    }
#else
    (void)onFileFound;
    (void)onProgress;
    (void)isRunning;
#endif
}

void scanVssSnapshotsBound(DiskReader& evidence,
                           FileSystemParser::FileRecordCallback onFileFound,
                           VssScanProgressFn onProgress,
                           std::atomic<bool>* isRunning) {
#ifdef _WIN32
    auto ids = collectVolumeIdentities(evidence);
    if (ids.empty()) return;

    int matched = 0;
    int collisions = 0;
    for (const auto& snap : enumerateVssSnapshots()) {
        if (isRunning && !(*isRunning)) break;
        DiskReader vssReader;
        if (!vssReader.openVolumePath(snap.devicePath)) continue;
        uint32_t ss = vssReader.getSectorSize();
        if (ss == 0) ss = 512;
        std::vector<uint8_t> boot(ss);
        if (!vssReader.readSectors(0, ss, boot.data()).success) continue;
        uint64_t ser = parseVolumeSerial(boot.data(), boot.size());
        if (ser == 0) continue;
        int hits = 0;
        for (const auto& id : ids) {
            if (volumeIdentityMatches(id, ser, snap.sizeBytes)) ++hits;
        }
        if (hits == 0) continue;
        if (hits > 1) ++collisions;

        FileRecord meta;
        meta.id = -1;
        meta.name = "VSS_Snapshot_" + std::to_string(snap.index);
        meta.path = snap.devicePath;
        meta.sizeBytes = snap.sizeBytes;
        meta.status = 1;
        meta.confidence = 95;
        meta.category = "System";
        meta.source = "vss_snapshot";
        meta.createdAt = snap.createdAt > 0 ? snap.createdAt : snap.discoveredAt;
        onFileFound(meta);
        scanVssVolumeFilesystem(vssReader, snap, onFileFound, onProgress, isRunning);
        ++matched;
    }
    if (matched > 0) {
        FileRecord fr;
        fr.id = -1;
        fr.name = collisions > 0
            ? "[VSS] " + std::to_string(matched) + " snapshot(s); serial collision on "
              + std::to_string(collisions)
            : "[VSS] " + std::to_string(matched) + " snapshot(s) bound by volume serial+size";
        fr.path = "/";
        fr.status = 1;
        fr.confidence = 100;
        fr.category = "Metadata";
        fr.source = "vss_bind";
        onFileFound(fr);
    }
#else
    (void)evidence;
    (void)onFileFound;
    (void)onProgress;
    (void)isRunning;
#endif
}

std::string vssDevicePathFromRecord(const FileRecord& rec) {
    if (rec.source != "vss_ntfs" && rec.source != "vss_fat") return {};
    const std::string& p = rec.path;
    if (p.find("HarddiskVolumeShadowCopy") != std::string::npos) return p;
    if (p.size() < 5 || p.compare(0, 4, "/VSS") != 0) return {};
    size_t i = 4;
    if (i >= p.size() || p[i] < '0' || p[i] > '9') return {};
    int idx = 0;
    while (i < p.size() && p[i] >= '0' && p[i] <= '9') {
        idx = idx * 10 + (p[i] - '0');
        ++i;
        if (idx > 256) return {};
    }
    if (idx <= 0) return {};
    return "\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy" + std::to_string(idx);
}

} // namespace wolf
