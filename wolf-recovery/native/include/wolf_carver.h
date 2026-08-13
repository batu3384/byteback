#pragma once

#include "wolf_db.h"
#include "wolf_io.h"
#include <functional>
#include "wolf_fs.h"
#include <vector>
#include <string>
#include <atomic>

#include <queue>
#include <map>
#include <unordered_map>
#include <memory>

namespace wolf {

struct FileSignature {
    int id; // Used to uniquely identify signatures in the state machine
    std::string format;
    std::string extension;
    std::string category;
    std::vector<uint8_t> header;
    std::vector<uint8_t> footer;
    uint64_t maxSize;
};

class EntropyAnalyzer {
public:
    static double calculateShannonEntropy(const uint8_t* buffer, size_t bufferSize, size_t offset, size_t length);
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
    bool scan(DiskReader& reader, FileSystemParser::FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr);

private:
    std::vector<FileSignature> signatures;
    std::vector<ACTrieNode> acNodes;
    
    std::vector<uint8_t> hexToBytes(const std::string& hex);
    
    void buildAhoCorasick();
};

} // namespace wolf

