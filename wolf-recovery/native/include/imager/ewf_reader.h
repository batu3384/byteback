#pragma once

#include "io/byte_source.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wolf {

// Read uncompressed EWF1 images produced by EwfWriter (local path or http(s) URL).
class EwfReader {
public:
    EwfReader();
    ~EwfReader();

    bool open(const std::string& pathOrUrl, std::string& err);
    void close();

    bool isOpen() const { return bytesPerSector_ > 0 && imageBytes_ > 0; }
    uint64_t imageBytes() const { return imageBytes_; }
    uint32_t bytesPerSector() const { return bytesPerSector_; }
    uint64_t totalSectors() const {
        return bytesPerSector_ ? imageBytes_ / bytesPerSector_ : 0;
    }
    std::string md5Hex() const { return md5Hex_; }

    bool read(uint64_t offsetBytes, uint8_t* buf, size_t len, std::string& err);

private:
    struct SegmentMap {
        std::unique_ptr<ByteSource> source;
        uint64_t sectorsDataOffset = 0;
        uint64_t sectorsDataBytes = 0;
        uint64_t imageBaseOffset = 0;
    };

    bool parseAllSegments(const std::string& firstPath, std::string& err);
    bool parseSegment(ByteSource& src, bool firstSegment, SegmentMap& out, std::string& err);

    uint32_t bytesPerSector_ = 0;
    uint64_t imageBytes_ = 0;
    std::string md5Hex_;
    std::vector<SegmentMap> segments_;
    std::string basePath_;
};

} // namespace wolf
