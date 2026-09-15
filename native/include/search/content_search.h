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
    // CA-031: treat `query` as an ECMAScript regex (matched case-insensitively,
    // first match wins) instead of a case-insensitive literal. The FTS
    // shortcut cannot evaluate regexes, so the search always walks the disk in
    // this mode. Unsafe/invalid/oversized patterns are refused (status 5),
    // not walked as a literal and not reported as zero hits.
    bool useRegex = false;
};

using ContentMatchCallback = std::function<void(const FileRecord&)>;
using ContentProgressCallback = std::function<void(uint64_t current, uint64_t total)>;
/** 1=complete, 2=stopped, 3=reader open failed (not empty hits),
 *  4=query longer than kMaxContentQueryBytes (overlap would silently miss),
 *  5=regex refused (invalid, nested-quantifier/ReDoS shape, or >128 chars),
 *  6=unread I/O during the walk (not a clean complete; empty ≠ no match). */
constexpr int kContentSearchComplete = 1;
constexpr int kContentSearchStopped = 2;
constexpr int kContentSearchOpenFailed = 3;
constexpr int kContentSearchQueryTooLong = 4;
constexpr int kContentSearchRegexRejected = 5;
constexpr int kContentSearchReadIncomplete = 6;
constexpr size_t kMaxContentQueryBytes = 64u * 1024u;
constexpr size_t kMaxContentRegexChars = 128;
using ContentFinishedCallback = std::function<void(int status)>;

/** True when `pattern` is safe to compile as the content-search regex. */
bool contentRegexQueryOk(const std::string& pattern);

/** 0 = walk; otherwise a kContentSearch* refuse code (not a silent empty hit). */
inline int contentSearchWalkStatus(const std::string& query,
                                   const ContentSearchOptions& opts = {}) {
    if (query.size() > kMaxContentQueryBytes) return kContentSearchQueryTooLong;
    if (opts.useRegex && !contentRegexQueryOk(query)) return kContentSearchRegexRejected;
    return 0;
}

std::string sanitizeContentSample(const std::vector<uint8_t>& raw, uint64_t maxLen = 256 * 1024);

// CA-031 snippet-window sanitizer: maps every input byte 1:1 (kept byte or
// '.') so byte offsets computed before sanitization stay valid after it —
// the invariant the renderer's byte-offset highlight (highlight.ts
// byteOffsetToUnitIndex) relies on. Mostly-binary context (<80% printable)
// also replaces bytes >=128 with '.'.
std::string sanitizeSnippetContext(const std::string& raw);

std::vector<FileRecord> searchFileContent(MetadataStore& store, DiskReader& reader,
                                          int64_t scanId, const std::string& query,
                                          int offset, int limit,
                                          const ContentSearchOptions& opts = {});

int64_t searchFileContentCount(MetadataStore& store, DiskReader& reader,
                               int64_t scanId, const std::string& query,
                               const ContentSearchOptions& opts = {});

int runContentSearch(MetadataStore& store, DiskReader& reader,
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
