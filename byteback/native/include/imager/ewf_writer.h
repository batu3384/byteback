#pragma once

// EWF (Expert Witness Format, .E01) writer — the de facto legal-standard
// forensic image format readable by EnCase, FTK, X-Ways, Autopsy (libewf).
//
// Uncompressed EWF1. Table offsets are uint32, so each segment's sectors
// section stays under 4 GiB; the writer rotates .E01 → .E02 → … and patches
// total_segments in every file header on finish. zlib chunks remain a
// drop-in (same table format).
//
// CA-004 verification status: container round-trips in test_ewf.cpp. Optional
// independent check: set BYTEBACK_EWFINFO to an ewfinfo binary (CI skips if absent).

#include "crypto/byteback_md5.h"
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

namespace byteback {

struct EwfOptions {
    // 128 sectors * 512 B = 64 KiB per chunk — matches the libewf default.
    uint32_t sectorsPerChunk = 128;
    std::string caseNumber = "case1";
    std::string evidenceNumber = "evidence1";
    std::string examiner = "Byteback";
    std::string notes;
    // Serial string embedded in the header section.
    std::string serial = "BYTEBACK01";
};

class EwfWriter {
public:
    EwfWriter();
    ~EwfWriter();

    // Prepare the destination file and write the fixed header sections.
    // destPath is the .E01 path; bytesPerSector is typically 512.
    bool open(const std::string& destPath,
              uint64_t totalSectors,
              uint32_t bytesPerSector,
              const EwfOptions& opts = EwfOptions());

    // Append sector-aligned image data. Length must be a multiple of
    // bytesPerSector; the caller feeds data in any convenient block sizes
    // (the writer repacks it into chunks internally).
    bool write(const uint8_t* data, size_t length);

    // Emit the table/digest/done sections and close the file.
    bool finish();

    bool isOpen() const { return outFile_.is_open(); }
    // MD5 over the image data (hex). Populated by finish(); empty before.
    std::string md5Hex() const { return finishedMd5Hex_; }
    uint64_t bytesWritten() const { return bytesWritten_; }
    // Test hook: rotate before the uint32 table ceiling. Default 0xFFFF0000.
    void setMaxSectorsSectionBytes(uint64_t n) { maxSectorsSectionBytes_ = n; }
    int segmentCount() const { return static_cast<int>(segmentPaths_.size()); }

private:
    void writeSectionHeader(const char type[16], uint64_t size);
    bool startSegment(int number, bool first);
    bool closeSegment(bool last);
    bool rotateSegment();
    std::string segmentPathFor(int number) const;
    void patchSegmentFileHeader(const std::string& path, uint16_t number, uint16_t total);

    // Read/write stream: the writer must be able to read the file header
    // back to compute its checksum (MD5 over the first 0x44 bytes) and to
    // patch the sectors-section size once the data size is known.
    std::fstream outFile_;
    EwfOptions opts_;
    uint32_t bytesPerSector_ = 512;
    uint64_t totalSectors_ = 0;
    uint64_t bytesWritten_ = 0;       // image bytes fed via write()
    std::string finishedMd5Hex_;      // set by finish()
    std::vector<uint32_t> chunkOffsets_; // byte offset of each chunk in the sectors section

    crypto::Md5 imageMd5_;
    uint64_t sectorsDataStart_ = 0;   // file offset where chunk data begins
    uint64_t currentChunkBytes_ = 0;  // bytes written into the current sectors section
    uint64_t maxSectorsSectionBytes_ = 0xFFFF0000ull;
    std::string destPath_;
    int segmentNumber_ = 1;
    std::vector<std::string> segmentPaths_;
};

} // namespace byteback
