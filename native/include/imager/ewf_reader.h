#pragma once

#include "io/byte_source.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace byteback {

// Read EWF1 images produced by EwfWriter (local path or http(s) URL).
// Handles both stored (uncompressed) chunks and zlib-deflate compressed
// chunks (C3): the sectors/table section headers carry the compression flag
// in reserved byte 32; table entries are byte counts of the chunk extents
// inside the sectors section. Uncompressed images keep the legacy linear
// read path — older files (and aborted acquisitions) read exactly as before.
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

    // CA-039: verify the stored MD5 digest by hashing the image data during a
    // full sequential read and comparing in constant time. Returns false when
    // no digest is stored (aborted acquisitions), on any read failure, or on
    // a mismatch.
    bool verifyDigest();

    // D2: acquisition read-error runs from the "error" section, as
    // {startSector, sectorCount} pairs. Empty when the image carries none.
    const std::vector<std::pair<uint64_t, uint64_t>>& acquiryErrors() const {
        return acquiryErrors_;
    }

private:
    struct SegmentMap {
        std::unique_ptr<ByteSource> source;
        uint64_t sectorsDataOffset = 0;
        uint64_t sectorsDataBytes = 0;
        uint64_t imageBaseOffset = 0;
        // C3: 0 = stored chunks (legacy linear layout), 1 = raw deflate.
        uint8_t chunkCompression = 0;
        // Chunk start byte counts inside the sectors section (last extent
        // ends at sectorsDataBytes). Empty for legacy linear images.
        std::vector<uint32_t> chunkTable;
        // Single-chunk plaintext cache: carve/verify reads walk sequentially.
        mutable uint64_t cachedChunk = UINT64_MAX;
        mutable std::vector<uint8_t> cachedData;
    };

    bool parseAllSegments(const std::string& firstPath, std::string& err);
    bool parseSegment(ByteSource& src, bool firstSegment, SegmentMap& out, std::string& err);
    // Inflate chunk `idx` of `seg` into the segment cache; returns false with
    // err set on any compressed-extent/deflate failure.
    bool inflateChunk(SegmentMap& seg, uint64_t idx, std::string& err) const;

    uint32_t bytesPerSector_ = 0;
    uint32_t sectorsPerChunk_ = 0;
    uint64_t imageBytes_ = 0;
    std::string md5Hex_;
    std::vector<SegmentMap> segments_;
    std::vector<std::pair<uint64_t, uint64_t>> acquiryErrors_;
    std::string basePath_;
};

} // namespace byteback
