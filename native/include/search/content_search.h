#pragma once

#include "byteback_db.h"
#include "byteback_io.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace byteback {

class VirtualRaid;

struct ContentSearchOptions {
    uint64_t chunkBytes = 256 * 1024;
};

using ContentMatchCallback = std::function<void(const FileRecord&)>;
using ContentProgressCallback = std::function<void(uint64_t current, uint64_t total)>;
// status: 1=complete, 2=stopped
using ContentFinishedCallback = std::function<void(int status)>;

std::string sanitizeContentSample(const std::vector<uint8_t>& raw, uint64_t maxLen = 256 * 1024);

std::vector<FileRecord> searchFileContent(MetadataStore& store, DiskReader& reader,
                                          int64_t scanId, const std::string& query,
                                          int offset, int limit,
                                          const ContentSearchOptions& opts = {});

int64_t searchFileContentCount(MetadataStore& store, DiskReader& reader,
                               int64_t scanId, const std::string& query,
                               const ContentSearchOptions& opts = {});

void runContentSearch(MetadataStore& store, DiskReader& reader,
                      int64_t scanId, const std::string& query,
                      const ContentSearchOptions& opts,
                      ContentMatchCallback onMatch,
                      ContentProgressCallback onProgress,
                      std::atomic<bool>* isRunning);

class ContentSearchCoordinator {
public:
    void startSearch(MetadataStore& store, int driveIndex, std::shared_ptr<VirtualRaid> raid,
                     int64_t scanId, const std::string& query,
                     const ContentSearchOptions& opts,
                     ContentMatchCallback onMatch,
                     ContentProgressCallback onProgress,
                     ContentFinishedCallback onFinished,
                     const DiskReader* fvekSource = nullptr);
    void requestStop();
    void stopSearch();

private:
    std::thread worker_;
    std::atomic<bool> running_{false};
};

} // namespace byteback
