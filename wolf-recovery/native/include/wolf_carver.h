#pragma once

#include "wolf_db.h"
#include "wolf_io.h"
#include <functional>
#include <vector>
#include <string>

namespace wolf {

struct FileSignature {
    std::string format;
    std::string extension;
    std::string category;
    std::vector<uint8_t> header;
    std::vector<uint8_t> footer;
    uint64_t maxSize;
};

class CarvingEngine {
public:
    CarvingEngine();
    ~CarvingEngine();

    bool loadSignatures(const std::string& jsonPath);
    bool scan(DiskReader& reader, FileSystemParser::FileRecordCallback callback);

private:
    std::vector<FileSignature> signatures;
    
    std::vector<uint8_t> hexToBytes(const std::string& hex);
};

} // namespace wolf
