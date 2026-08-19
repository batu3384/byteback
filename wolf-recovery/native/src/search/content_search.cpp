#include "search/content_search.h"
#include "fs/virtual_raid.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

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

bool readFileSample(DiskReader& reader, const FileRecord& rec, uint64_t maxBytes,
                    std::vector<uint8_t>& buf) {
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    uint64_t toRead = std::min(maxBytes, rec.sizeBytes > 0 ? rec.sizeBytes : maxBytes);
    buf.clear();
    buf.resize(static_cast<size_t>(toRead), 0);

    if (!rec.runs.empty()) {
        uint64_t filled = 0;
        uint64_t allocSize = 0;
        for (const auto& run : rec.runs) {
            if (filled >= toRead) break;
            uint64_t runBytes = run.sectorCount * sectorSize;
            uint64_t take = std::min(runBytes, toRead - filled);
            allocSize += ((take + sectorSize - 1) / sectorSize) * sectorSize;
            filled += take;
        }
        buf.resize(static_cast<size_t>(allocSize), 0);
        filled = 0;
        for (const auto& run : rec.runs) {
            if (filled >= toRead) break;
            uint64_t runBytes = run.sectorCount * sectorSize;
            uint64_t take = std::min(runBytes, toRead - filled);
            uint32_t readBytes = static_cast<uint32_t>(((take + sectorSize - 1) / sectorSize) * sectorSize);
            auto res = reader.readSectors(run.startSector * sectorSize, readBytes, buf.data() + filled);
            if (!res.success) std::memset(buf.data() + filled, 0, static_cast<size_t>(take));
            filled += take;
        }
        buf.resize(static_cast<size_t>(toRead));
        return toRead > 0;
    }

    if (rec.endSector > rec.startSector) {
        uint32_t sectorSize = reader.getSectorSize();
        if (sectorSize == 0) sectorSize = 512;
        uint64_t span = (rec.endSector - rec.startSector) * sectorSize;
        toRead = std::min(toRead, span);
        uint32_t readBytes = static_cast<uint32_t>(((toRead + sectorSize - 1) / sectorSize) * sectorSize);
        buf.resize(readBytes, 0);
        auto res = reader.readSectors(rec.startSector * sectorSize, readBytes, buf.data());
        if (res.success) buf.resize(static_cast<size_t>(toRead));
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
            if (f.sizeBytes > opts.maxFileSize) {
                ++processed;
                if (onProgress) onProgress(static_cast<uint64_t>(processed), static_cast<uint64_t>(total));
                continue;
            }

            std::string text = store.getContentSample(f.id);
            if (text.empty()) {
                std::vector<uint8_t> sample;
                if (readFileSample(reader, f, opts.maxBytesPerFile, sample)) {
                    text = sanitizeContentSample(sample, opts.maxBytesPerFile);
                    if (!text.empty()) store.upsertContentSample(scanId, f.id, text);
                }
            }

            if (!text.empty() && icontains(text, query)) {
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

void ContentSearchCoordinator::stopSearch() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
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
