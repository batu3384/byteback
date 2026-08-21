#pragma once

#include "byteback_db.h"
#include "byteback_io.h"
#include <cstring>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace byteback::ntfs {

inline bool isThumbcacheDbName(const std::string& name) {
    if (name.size() < 10) return false;
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower.find("thumbcache") != std::string::npos && lower.find(".db") != std::string::npos;
}

inline void emitEmbeddedJpegs(const uint8_t* data, size_t n, const FileRecord& parent, int64_t& nextId,
                                const std::function<void(const FileRecord&)>& callback) {
    if (!data || n < 4) return;
    size_t i = 0;
    while (i + 3 < n) {
        if (data[i] != 0xFF || data[i + 1] != 0xD8 || data[i + 2] != 0xFF) {
            ++i;
            continue;
        }
        size_t j = i + 3;
        while (j + 1 < n) {
            if (data[j] == 0xFF && data[j + 1] == 0xD9) {
                j += 2;
                break;
            }
            ++j;
        }
        if (j <= i + 3 || j > n) {
            ++i;
            continue;
        }
        FileRecord fr{};
        fr.id = nextId++;
        fr.parentId = parent.id;
        fr.name = parent.name + "_thumb_" + std::to_string(i) + ".jpg";
        fr.extension = ".jpg";
        if (!parent.path.empty()) fr.path = parent.path + "\\" + fr.name;
        else fr.path = fr.name;
        fr.sizeBytes = static_cast<uint64_t>(j - i);
        fr.status = 0;
        fr.source = "ntfs_thumbcache";
        fr.confidence = 42;
        fr.category = "Image";
        if (parent.startSector) fr.startSector = parent.startSector;
        callback(fr);
        i = j;
    }
}

inline bool readRecordPrefix(DiskReader& reader, const FileRecord& rec, uint64_t maxBytes,
                             std::vector<uint8_t>& out, std::atomic<bool>* isRunning) {
    out.clear();
    if (maxBytes == 0) return false;
    if (!rec.residentData.empty()) {
        const size_t take = static_cast<size_t>(std::min(maxBytes, rec.residentData.size()));
        out.assign(rec.residentData.begin(), rec.residentData.begin() + static_cast<std::ptrdiff_t>(take));
        return !out.empty();
    }

    const uint32_t sectorSize = reader.getSectorSize() ? reader.getSectorSize() : 512;

    uint64_t fileBytes = rec.sizeBytes;
    if (fileBytes == 0 && !rec.runs.empty()) {
        for (const auto& run : rec.runs)
            fileBytes += static_cast<uint64_t>(run.sectorCount) * sectorSize;
    }
    if (fileBytes == 0) return false;

    const uint64_t want = std::min(maxBytes, fileBytes);
    out.resize(static_cast<size_t>(want));
    uint64_t copied = 0;

    if (!rec.runs.empty()) {
        for (const auto& run : rec.runs) {
            if (isRunning && !*isRunning) return false;
            const uint64_t runBytes = static_cast<uint64_t>(run.sectorCount) * sectorSize;
            uint64_t offsetInRun = 0;
            while (offsetInRun < runBytes && copied < want) {
                const uint64_t chunk = std::min(runBytes - offsetInRun, want - copied);
                const uint32_t readSize =
                    static_cast<uint32_t>(((chunk + sectorSize - 1) / sectorSize) * sectorSize);
                std::vector<uint8_t> buf(readSize);
                const uint64_t byteOff = run.startSector * sectorSize + offsetInRun;
                auto res = reader.readSectors(byteOff, readSize, buf.data());
                if (!res.success || res.bytesRead == 0) {
                    offsetInRun = runBytes;
                    break;
                }
                const size_t take = static_cast<size_t>(std::min<uint64_t>(chunk, res.bytesRead));
                std::memcpy(out.data() + copied, buf.data(), take);
                copied += take;
                offsetInRun += chunk;
            }
            if (copied >= want) break;
        }
        out.resize(static_cast<size_t>(copied));
        return copied > 0;
    }

    if (rec.startSector == 0) return false;
    const uint32_t readSize =
        static_cast<uint32_t>(((want + sectorSize - 1) / sectorSize) * sectorSize);
    out.resize(readSize);
    auto res = reader.readSectors(rec.startSector * sectorSize, readSize, out.data());
    if (!res.success || res.bytesRead == 0) return false;
    out.resize(static_cast<size_t>(std::min<uint64_t>(want, res.bytesRead)));
    return true;
}

inline void emitThumbcacheThumbnails(DiskReader& reader, const FileRecord& parent, int64_t& nextId,
                                     const std::function<void(const FileRecord&)>& callback,
                                     std::atomic<bool>* isRunning) {
    if (!isThumbcacheDbName(parent.name)) return;
  // ponytail: 128 MiB cap — enough for thumbcache_256.db prefix on typical volumes.
    constexpr uint64_t kCap = 128u << 20;
    std::vector<uint8_t> buf;
    if (!readRecordPrefix(reader, parent, kCap, buf, isRunning)) return;
    emitEmbeddedJpegs(buf.data(), buf.size(), parent, nextId, callback);
}

} // namespace byteback::ntfs
