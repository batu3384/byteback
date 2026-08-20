#include "search/content_search.h"
#include "fs/virtual_raid.h"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

namespace wolf {

namespace {

bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != hay.end();
}

bool readFileRange(DiskReader& reader, const FileRecord& rec, uint64_t byteOff, uint64_t maxBytes,
                    std::vector<uint8_t>& buf) {
    buf.clear();
    if (maxBytes == 0) return false;
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    if (!rec.residentData.empty()) {
        if (byteOff >= rec.residentData.size()) return false;
        uint64_t take = std::min(maxBytes, rec.residentData.size() - byteOff);
        buf.assign(rec.residentData.begin() + static_cast<std::ptrdiff_t>(byteOff),
                   rec.residentData.begin() + static_cast<std::ptrdiff_t>(byteOff + take));
        return true;
    }

    auto copyFromRuns = [&](uint64_t wantOff, uint64_t wantLen) -> bool {
        uint64_t logical = 0;
        uint64_t filled = 0;
        buf.assign(static_cast<size_t>(wantLen), 0);
        for (const auto& run : rec.runs) {
            if (filled >= wantLen) break;
            uint64_t runBytes = run.sectorCount * sectorSize;
            if (logical + runBytes <= wantOff) {
                logical += runBytes;
                continue;
            }
            uint64_t skipInRun = wantOff > logical ? wantOff - logical : 0;
            uint64_t avail = runBytes - skipInRun;
            uint64_t take = std::min(avail, wantLen - filled);
            uint64_t readOff = run.startSector * sectorSize + skipInRun;
            uint32_t readBytes = static_cast<uint32_t>(((take + sectorSize - 1) / sectorSize) * sectorSize);
            std::vector<uint8_t> tmp(readBytes, 0);
            auto res = reader.readSectors(readOff, readBytes, tmp.data());
            if (res.success) {
                std::memcpy(buf.data() + filled, tmp.data(), static_cast<size_t>(take));
            }
            filled += take;
            logical += runBytes;
        }
        buf.resize(static_cast<size_t>(filled));
        return filled > 0;
    };

    if (!rec.runs.empty()) {
        uint64_t total = rec.sizeBytes;
        if (total == 0) {
            for (const auto& run : rec.runs) total += run.sectorCount * sectorSize;
        }
        if (byteOff >= total) return false;
        uint64_t take = std::min(maxBytes, total - byteOff);
        return copyFromRuns(byteOff, take);
    }

    if (rec.endSector > rec.startSector) {
        uint64_t span = (rec.endSector - rec.startSector) * sectorSize;
        if (byteOff >= span) return false;
        uint64_t take = std::min(maxBytes, span - byteOff);
        uint64_t readOff = rec.startSector * sectorSize + byteOff;
        uint32_t readBytes = static_cast<uint32_t>(((take + sectorSize - 1) / sectorSize) * sectorSize);
        buf.assign(readBytes, 0);
        auto res = reader.readSectors(readOff, readBytes, buf.data());
        if (res.success) buf.resize(static_cast<size_t>(take));
        return res.success;
    }
    return false;
}

void tagContentMatch(FileRecord& f) {
    f.source = f.source.empty() ? "content_match" : f.source + "+content";
}

} // namespace

std::string sanitizeContentSample(const std::vector<uint8_t>& raw, uint64_t maxLen) {
    std::string out;
    out.reserve(std::min<uint64_t>(raw.size(), maxLen));
    for (size_t i = 0; i < raw.size() && out.size() < maxLen; ++i) {
        unsigned char c = raw[i];
        if (c == '\t' || c == '\n' || c == '\r' || (c >= 32 && c < 127)) {
            out.push_back(static_cast<char>(c));
        } else if (c >= 128) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back(' ');
        }
    }
    return out;
}

void runContentSearch(MetadataStore& store, DiskReader& reader,
                      int64_t scanId, const std::string& query,
                      const ContentSearchOptions& opts,
                      ContentMatchCallback onMatch,
                      ContentProgressCallback onProgress,
                      std::atomic<bool>* isRunning) {
    if (query.empty() || scanId <= 0) return;

    int64_t total = store.getFileCount(scanId);
    if (total <= 0) {
        if (onProgress) onProgress(0, 0);
        return;
    }

    if (store.isContentIndexComplete(scanId)) {
        auto ids = store.searchContentFts(scanId, query, 0, 100000);
        int64_t processed = 0;
        for (int64_t id : ids) {
            if (isRunning && !(*isRunning)) break;
            FileRecord f = store.getFileById(id);
            if (f.id > 0) {
                tagContentMatch(f);
                onMatch(f);
            }
            ++processed;
            if (onProgress) onProgress(processed, static_cast<uint64_t>(ids.size()));
        }
        if (onProgress) onProgress(static_cast<uint64_t>(ids.size()), static_cast<uint64_t>(ids.size()));
        return;
    }

    int64_t processed = 0;
    const int page = 200;
    for (int64_t off = 0; off < total; off += page) {
        if (isRunning && !(*isRunning)) break;
        auto files = store.getFiles(scanId, static_cast<int>(off), page);
        for (auto& f : files) {
            if (isRunning && !(*isRunning)) break;
            const uint64_t chunk = opts.chunkBytes > 0 ? opts.chunkBytes : (256ull * 1024ull);
            uint64_t fileBytes = f.sizeBytes;
            if (fileBytes == 0 && !f.residentData.empty()) fileBytes = f.residentData.size();
            if (fileBytes == 0) {
                uint32_t ss = reader.getSectorSize();
                if (ss == 0) ss = 512;
                for (const auto& run : f.runs) fileBytes += run.sectorCount * ss;
            }
            if (fileBytes == 0 && f.endSector > f.startSector) {
                uint32_t ss = reader.getSectorSize();
                if (ss == 0) ss = 512;
                fileBytes = (f.endSector - f.startSector) * ss;
            }

            std::vector<std::string> chunks;
            if (fileBytes > 0) {
                for (uint64_t o = 0; o < fileBytes; o += chunk) {
                    if (isRunning && !(*isRunning)) break;
                    std::vector<uint8_t> sample;
                    if (!readFileRange(reader, f, o, chunk, sample)) break;
                    std::string t = sanitizeContentSample(sample, chunk);
                    if (!t.empty()) chunks.push_back(std::move(t));
                }
            }
            if (!chunks.empty()) store.replaceContentChunks(f.id, chunks);

            bool hit = false;
            for (const auto& t : chunks) {
                if (icontains(t, query)) { hit = true; break; }
            }
            if (hit) {
                tagContentMatch(f);
                onMatch(f);
            }

            ++processed;
            if (onProgress) onProgress(static_cast<uint64_t>(processed), static_cast<uint64_t>(total));
        }
    }
}

int64_t searchFileContentCount(MetadataStore& store, DiskReader& reader,
                               int64_t scanId, const std::string& query,
                               const ContentSearchOptions& opts) {
    if (query.empty() || scanId <= 0) return 0;
    int64_t matches = 0;
    std::atomic<bool> running{true};
    runContentSearch(store, reader, scanId, query, opts,
                     [&](const FileRecord&) { ++matches; },
                     nullptr, &running);
    return matches;
}

std::vector<FileRecord> searchFileContent(MetadataStore& store, DiskReader& reader,
                                          int64_t scanId, const std::string& query,
                                          int offset, int limit,
                                          const ContentSearchOptions& opts) {
    std::vector<FileRecord> out;
    if (query.empty() || scanId <= 0 || limit <= 0) return out;

    int skipped = 0;
    std::atomic<bool> running{true};
    runContentSearch(store, reader, scanId, query, opts,
                     [&](const FileRecord& f) {
                         if (skipped < offset) {
                             ++skipped;
                             return;
                         }
                         if (static_cast<int>(out.size()) < limit) out.push_back(f);
                     },
                     nullptr, &running);
    return out;
}

void ContentSearchCoordinator::requestStop() {
    running_ = false;
}

void ContentSearchCoordinator::stopSearch() {
    requestStop();
    if (!worker_.joinable()) return;
    if (worker_.get_id() == std::this_thread::get_id()) {
        worker_.detach();
        return;
    }
    worker_.join();
}

void ContentSearchCoordinator::startSearch(MetadataStore& store, int driveIndex,
                                           std::shared_ptr<VirtualRaid> raid,
                                           int64_t scanId, const std::string& query,
                                           const ContentSearchOptions& opts,
                                           ContentMatchCallback onMatch,
                                           ContentProgressCallback onProgress,
                                           ContentFinishedCallback onFinished) {
    stopSearch();
    running_ = true;
    worker_ = std::thread([this, &store, driveIndex, raid = std::move(raid), scanId, query, opts,
                          onMatch = std::move(onMatch), onProgress = std::move(onProgress),
                          onFinished = std::move(onFinished)]() mutable {
        DiskReader reader;
        if (raid) {
            reader.setRaidBackend(std::move(raid));
        } else if (driveIndex < 0 || !reader.openDrive(driveIndex)) {
            if (onFinished) onFinished(3);
            running_ = false;
            return;
        }
        runContentSearch(store, reader, scanId, query, opts, onMatch, onProgress, &running_);
        if (onFinished) onFinished(running_.load() ? 1 : 2);
        running_ = false;
    });
}

} // namespace wolf
