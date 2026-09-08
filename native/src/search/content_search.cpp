#include "search/content_search.h"
#include "fs/virtual_raid.h"
#include "fs/vss_scanner.h"
#include "byteback_recovery.h"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <memory>
#include <regex>
#include <vector>

namespace byteback {

namespace {

// ReDoS guard, mirroring the renderer's isSafeHighlightRegex policy
// (src/renderer/components/SearchView/highlight.ts): reject patterns where a
// quantifier is applied to a group whose body contains a quantifier or an
// alternation — the classic exponential-backtracking shape ("(a+)+$"). Such
// patterns fall back to literal search, same as oversized patterns. This is a
// conservative shape match: some safe patterns degrade to literal, an
// acceptable trade for never hanging the main process on hostile input.
bool regexLikelyBacktracking(const std::string& pattern) {
    // Locate each quantifier applied to a ')'; inspect the matching group body.
    for (size_t i = 1; i < pattern.size(); ++i) {
        if (pattern[i] != ')' ) continue;
        if (i + 1 >= pattern.size()) break;
        char q = pattern[i + 1];
        bool quantified = q == '*' || q == '+' ||
                          (q == '{' && pattern.find('}', i + 2) != std::string::npos);
        if (!quantified) continue;
        // Find the matching '(' for this ')' (plain depth scan; character
        // classes with parens degrade to literal — never a security loss).
        int depth = 0;
        size_t open = std::string::npos;
        for (size_t j = 0; j <= i; ++j) {
            if (pattern[j] == '(') {
                if (depth == 0) open = j;
                ++depth;
            } else if (pattern[j] == ')') {
                if (depth > 0) --depth;
            }
        }
        if (open == std::string::npos) continue;
        for (size_t k = open + 1; k < i; ++k) {
            if (pattern[k] == '*' || pattern[k] == '+' || pattern[k] == '|') return true;
        }
    }
    return false;
}

// CA-031: single compiled matcher per search run — the chunk scan calls
// find() on every chunk, so a regex must compile once, not per chunk.
class QueryMatcher {
public:
    static QueryMatcher literal(std::string needle) {
        QueryMatcher m;
        m.literal_ = std::move(needle);
        return m;
    }
    static QueryMatcher regex(const std::string& pattern) {
        QueryMatcher m;
        if (pattern.size() > kMaxRegexQueryChars) {
            // Mirrors the IPC regex cap (ipc-handlers.ts): oversized patterns
            // fall back to literal search to bound backtracking exposure.
            m.literal_ = pattern;
            return m;
        }
        if (regexLikelyBacktracking(pattern)) {
            // Exponential-backtracking shape: literal fallback, never compile.
            m.literal_ = pattern;
            return m;
        }
        try {
            m.re_ = std::regex(pattern, std::regex::icase | std::regex::optimize);
            m.regex_ = true;
        } catch (const std::regex_error&) {
            // Invalid pattern matches nothing; find() never throws.
        }
        return m;
    }

    // First (leftmost) match position; *matchLen receives its length.
    // npos = no match.
    size_t find(const std::string& hay, size_t* matchLen) const {
        *matchLen = 0;
        if (regex_) {
            std::smatch m;
            if (!std::regex_search(hay, m, re_)) return std::string::npos;
            if (m.length(0) == 0) {
                // Zero-width match (e.g. "Z*"): the empty string matches at
                // every position, so honoring it would turn every file into a
                // hit with a 0-length span. Parity with the renderer's
                // zero-width guard: not a match.
                return std::string::npos;
            }
            *matchLen = static_cast<size_t>(m.length(0));
            return static_cast<size_t>(m.position(0));
        }
        if (literal_.empty()) return std::string::npos;
        auto it = std::search(hay.begin(), hay.end(), literal_.begin(), literal_.end(),
                              [](char a, char b) {
                                  return std::tolower(static_cast<unsigned char>(a)) ==
                                         std::tolower(static_cast<unsigned char>(b));
                              });
        if (it == hay.end()) return std::string::npos;
        *matchLen = literal_.size();
        return static_cast<size_t>(it - hay.begin());
    }

private:
    static constexpr size_t kMaxRegexQueryChars = 128;
    std::string literal_;
    std::regex re_;
    bool regex_ = false;
};

// CA-031 snippet protocol: ~160 bytes of context centered on the first match,
// plus byte offsets of the match span inside the snippet (post-sanitization;
// sanitization maps every input byte 1:1, so offsets survive it).
constexpr size_t kSnippetContextBytes = 160;

void attachSnippet(FileRecord& f, const std::string& chunkText, size_t matchPos, size_t matchLen) {
    if (chunkText.empty()) return;
    size_t begin = 0;
    if (chunkText.size() > kSnippetContextBytes) {
        const size_t center = matchPos + matchLen / 2;
        begin = center > kSnippetContextBytes / 2 ? center - kSnippetContextBytes / 2 : 0;
        if (begin + kSnippetContextBytes > chunkText.size()) {
            begin = chunkText.size() - kSnippetContextBytes;
        }
    }
    const size_t len = std::min(kSnippetContextBytes, chunkText.size() - begin);
    f.snippet = sanitizeSnippetContext(chunkText.substr(begin, len));
    // No span when the caller could not anchor the match (matchLen == 0) or
    // the context window clipped it.
    if (matchLen > 0 && matchPos >= begin && matchPos + matchLen <= begin + len) {
        f.snippetMatchStart = static_cast<int>(matchPos - begin);
        f.snippetMatchEnd = static_cast<int>(matchPos - begin + matchLen);
    }
}

// CA-031 FTS path: MATCH is token/prefix based — re-locate the query (or its
// first token) verbatim in the stored text to anchor the highlight. Only the
// first stored chunk is reachable through getContentSample; matches living in
// later chunks yield a head snippet without a highlight (known ceiling).
void attachFtsSnippet(MetadataStore& store, FileRecord& f, const std::string& query) {
    const std::string sample = store.getContentSample(f.id);
    if (sample.empty()) return;
    size_t mlen = 0;
    size_t pos = QueryMatcher::literal(query).find(sample, &mlen);
    if (pos == std::string::npos) {
        // Multi-token queries are ANDed by FTS; anchor on the first token.
        std::string token;
        for (size_t i = 0; i <= query.size() && pos == std::string::npos; ++i) {
            if (i == query.size() || std::isspace(static_cast<unsigned char>(query[i]))) {
                if (!token.empty()) {
                    pos = QueryMatcher::literal(token).find(sample, &mlen);
                    token.clear();
                }
            } else {
                token += query[i];
            }
        }
    }
    if (pos != std::string::npos) {
        attachSnippet(f, sample, pos, mlen);
    } else {
        attachSnippet(f, sample, 0, 0); // head snippet, no highlight span
    }
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

// Overlap between consecutive indexed chunks: a query (or FTS token) whose
// bytes straddle a chunk boundary must still match. ponytail: fixed 4 KiB
// overlap — phrases longer than this that straddle a boundary are still
// missed; upgrade path = query-length overlap at search time.
constexpr uint64_t kChunkOverlap = 4096;
// Max chunks held in memory before flushing to the store (bounds RAM on huge
// files: 32 x 256 KiB default chunks = ~8 MiB per file).
constexpr size_t kChunkFlushBatch = 32;

} // namespace

std::string sanitizeSnippetContext(const std::string& raw) {
    size_t printable = 0;
    for (unsigned char c : raw) {
        if (c == '\t' || c == '\n' || c == '\r' || (c >= 32 && c < 127)) ++printable;
    }
    // Binary-context guard: with <80% printable bytes, replace every non-text
    // byte ('.'), otherwise keep >=128 bytes untouched — valid UTF-8 is then
    // enforced at the trust boundary by the bridge's utf8ForJs.
    const bool aggressive = printable * 5 < raw.size() * 4;
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (c == '\t' || c == '\n' || c == '\r' || (c >= 32 && c < 127)) {
            out.push_back(static_cast<char>(c));
        } else if (!aggressive && c >= 128) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('.');
        }
    }
    return out;
}

// CA-031: byte offsets stay valid through sanitization (1:1 per byte); the
// renderer's highlight.ts byteOffsetToUnitIndex converts them to JS string
// indices exactly once. No bridge-side remapping — see byteback_db.h.
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

    // CA-031: the FTS shortcut cannot evaluate regexes — regex queries always
    // walk the disk (which also re-anchors snippets on live chunk text).
    if (!opts.useRegex && store.isContentIndexComplete(scanId)) {
        auto ids = store.searchContentFts(scanId, query, 0, 100000);
        int64_t processed = 0;
        for (int64_t id : ids) {
            if (isRunning && !(*isRunning)) break;
            FileRecord f = store.getFileById(id);
            if (f.id > 0) {
                tagContentMatch(f);
                attachFtsSnippet(store, f, query);
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
            if (isDiscoveryOnlySource(f.source)) {
                ++processed;
                if (onProgress) onProgress(static_cast<uint64_t>(processed), static_cast<uint64_t>(total));
                continue;
            }
            DiskReader* active = &reader;
            DiskReader vssReader;
            const std::string vssPath = vssDevicePathFromRecord(f);
            if (!vssPath.empty()) {
                if (!vssReader.openVolumePath(vssPath)) {
                    ++processed;
                    if (onProgress) onProgress(static_cast<uint64_t>(processed), static_cast<uint64_t>(total));
                    continue;
                }
                active = &vssReader;
            }
            const uint64_t chunk = opts.chunkBytes > 0 ? opts.chunkBytes : (256ull * 1024ull);
            uint64_t fileBytes = f.sizeBytes;
            if (fileBytes == 0 && !f.residentData.empty()) fileBytes = f.residentData.size();
            if (fileBytes == 0) {
                uint32_t ss = active->getSectorSize();
                if (ss == 0) ss = 512;
                for (const auto& run : f.runs) fileBytes += run.sectorCount * ss;
            }
            if (fileBytes == 0 && f.endSector > f.startSector) {
                uint32_t ss = active->getSectorSize();
                if (ss == 0) ss = 512;
                fileBytes = (f.endSector - f.startSector) * ss;
            }

            bool hit = false;
            const QueryMatcher matcher = opts.useRegex ? QueryMatcher::regex(query)
                                                       : QueryMatcher::literal(query);
            std::vector<std::string> chunks;
            bool flushed = false;
            auto flush = [&](bool force) {
                if (chunks.empty()) return true;
                if (!force && chunks.size() < kChunkFlushBatch) return true;
                bool ok = flushed ? store.appendContentChunks(f.id, chunks)
                                  : store.replaceContentChunks(f.id, chunks);
                flushed = true;
                chunks.clear();
                return ok;
            };
            if (fileBytes > 0) {
                for (uint64_t o = 0; o < fileBytes; o += chunk) {
                    if (isRunning && !(*isRunning)) break;
                    std::vector<uint8_t> sample;
                    if (!readFileRange(*active, f, o, chunk + kChunkOverlap, sample)) break;
                    std::string t = sanitizeContentSample(sample, chunk + kChunkOverlap);
                    if (!t.empty()) {
                        if (!hit) {
                            size_t mlen = 0;
                            const size_t mpos = matcher.find(t, &mlen);
                            if (mpos != std::string::npos) {
                                hit = true;
                                attachSnippet(f, t, mpos, mlen);
                            }
                        }
                        chunks.push_back(std::move(t));
                    }
                    flush(false);
                }
            }
            flush(true);
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
                                           ContentFinishedCallback onFinished,
                                           const DiskReader* fvekSource) {
    stopSearch();
    running_ = true;
    worker_ = std::thread([this, &store, driveIndex, raid = std::move(raid), scanId, query, opts,
                          onMatch = std::move(onMatch), onProgress = std::move(onProgress),
                          onFinished = std::move(onFinished), fvekSource]() mutable {
        DiskReader reader;
        if (raid) {
            reader.setRaidBackend(std::move(raid));
        } else if (driveIndex < 0 || !reader.openDrive(driveIndex)) {
            if (onFinished) onFinished(3);
            running_ = false;
            return;
        }
        if (fvekSource) reader.copyXtsFvekFrom(*fvekSource);
        runContentSearch(store, reader, scanId, query, opts, onMatch, onProgress, &running_);
        if (onFinished) onFinished(running_.load() ? 1 : 2);
        running_ = false;
    });
}

} // namespace byteback
