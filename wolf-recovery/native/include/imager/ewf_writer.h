#pragma once

// EWF (Expert Witness Format, .E01) writer — the de facto legal-standard
// forensic image format readable by EnCase, FTK, X-Ways, Autopsy (libewf).
//
// This writer emits a single-segment EWF1 stream with uncompressed chunks:
// the container structure (sections, table, MD5 digest) is fully spec-
// compliant, and uncompressed chunks are legal EWF — every major reader
// accepts them. zlib-compressed chunks are a drop-in extension once a zlib
// dependency is vendored (the table entry format is identical; a compressed
// chunk simply stores the deflate stream + adler32 instead of raw bytes).
//
// Stream layout produced:
//   [file header 76B]
//   [header section]   case/tool metadata (ASCII, NUL-terminated)
//   [disk section]     volume geometry (chunk count, sector sizes)
//   [sectors section]  all chunk data, back to back
//   [table section]    uint32 offset per chunk + base offset
//   [table2 section]   backup copy of the table
//   [digest section]   MD5 of the chunk data (16 raw bytes)
//   [done section]
//
// ponytail: single segment only — the sectors-section table entries are
// uint32, so images above ~4 GiB of chunk data would need either segmented
// output or multiple sectors sections. Upgrade path: rotate to .E02 when the
// sectors section approaches 4 GiB and set total_segments accordingly.
//
// CA-004 verification status: the container round-trips against OUR parser
// (test_ewf.cpp) but has NOT been cross-validated with an independent EWF
// reader (libewf ewfinfo/ewfverify, EnCase, FTK). The CI workflow carries an
// optional ewfinfo gate that skips when the tool is absent. Before relying on
// E01 output as evidence, run one image through an independent reader and
// compare the MD5.

#include "crypto/wolf_md5.h"
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

namespace wolf {

struct EwfOptions {
    // 128 sectors * 512 B = 64 KiB per chunk — matches the libewf default.
    uint32_t sectorsPerChunk = 128;
    std::string caseNumber = "case1";
    std::string evidenceNumber = "evidence1";
    std::string examiner = "Wolf Recovery";
    std::string notes;
    // Serial string embedded in the header section.
    std::string serial = "WOLF0001";
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

private:
    void writeSectionHeader(const char type[16], uint64_t size);

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
};

} // namespace wolf
