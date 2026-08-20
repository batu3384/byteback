#pragma once

#include "byteback_db.h"
#include "byteback_io.h"
#include <functional>
#include "byteback_fs.h"
#include <vector>
#include <string>
#include <atomic>

#include <queue>
#include <map>
#include <unordered_map>
#include <memory>

namespace byteback {

struct FileSignature {
    int id; // Used to uniquely identify signatures in the state machine
    std::string format;
    std::string extension;
    std::string category;
    std::vector<uint8_t> header;
    std::vector<uint8_t> footer;
    uint64_t maxSize;
};

struct ACTrieNode {
    std::map<uint8_t, int> children;
    int fail = 0;
    std::vector<int> headerMatches; // IDs of signatures where this node is the end of a header
    std::vector<int> footerMatches; // IDs of signatures where this node is the end of a footer
};

struct ActiveCarve {
    int sigId;
    uint64_t startOffset;
    uint64_t startSector;
    uint64_t endOffsetLimit;
    std::string filename;
};

class CarvingEngine {
public:
    CarvingEngine();
    ~CarvingEngine();

    bool loadSignatures(const std::string& jsonPath);
    size_t signatureCount() const { return signatures.size(); }
    bool scan(DiskReader& reader, FileSystemParser::FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr);
    bool scanRange(DiskReader& reader, uint64_t firstSector, uint64_t lastSector,
                   FileSystemParser::FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr);

    // ponytail: test hook; 0 = auto (hardware, max 4)
    void setCarveWorkerCount(unsigned count) { carveWorkers_ = count; }

private:
    bool scanRangeSingle(DiskReader& reader, uint64_t firstSector, uint64_t lastSector,
                         FileSystemParser::FileRecordCallback callback,
                         std::atomic<bool>* isRunning,
                         uint64_t emitFirstSector, uint64_t emitLastSector,
                         std::atomic<int>* bgcBudget);

    std::vector<FileSignature> signatures;
    std::vector<ACTrieNode> acNodes;
    uint32_t maxPatternBytes_ = 64;
    unsigned carveWorkers_ = 0;
    // CA-001: per-scan BGC budget — the rescue path is bounded so a stream of
    // partially-validated junk candidates can never dominate the scan time.
    int bgcBudget_ = 32;

    std::vector<uint8_t> hexToBytes(const std::string& hex);

    void buildAhoCorasick();
};

// Bifragmented Gap Carving (BGC).
//
// When a file is split into exactly two fragments on disk, header/footer
// carving finds the start and the end but cannot tell where the gap between
// the two fragments is. Garfinkel's BGC brute-forces every plausible gap
// position, reassembles the two fragments, and asks a type-specific validator
// whether the result is structurally sound. The first gap that validates wins.
//
// This is expensive (O(disk_size) candidate gaps per file) so it is reserved
// for the deep-scan path and capped by maxGapBytes. The validator is the same
// Fast Object Validation used elsewhere (carver/file_validators.h), so a JPEG
// with a correct marker stream or a ZIP with a valid central directory is
// accepted with high confidence.
//
// Result of a successful BGC search: the reassembly that validated consists
// of fragment 1 [headerOffset, headerOffset+frag1Len), a gap of gapLen
// bytes, then fragment 2 up to footerOffset.
struct BgcResult {
    bool found = false;
    size_t frag1Len = 0;
    size_t gapLen = 0;
    // ponytail: 3+ fragments — up to two internal gaps (three data spans).
    size_t frag2Len = 0;
    size_t gap2Len = 0;
};

BgcResult bifragmentedGapCarve(const uint8_t* disk, size_t diskSize,
                               size_t headerOffset, size_t footerOffset,
                               size_t maxGapBytes,
                               const std::function<int(const uint8_t*, size_t)>& validator,
                               size_t stepBytes = 1);

// ponytail: max two internal gaps (three fragments), bounded attempt budget.
BgcResult triFragmentedGapCarve(const uint8_t* disk, size_t diskSize,
                                size_t headerOffset, size_t footerOffset,
                                size_t maxGapBytes,
                                const std::function<int(const uint8_t*, size_t)>& validator,
                                size_t stepBytes = 1, size_t attemptBudget = 8192);

} // namespace byteback

