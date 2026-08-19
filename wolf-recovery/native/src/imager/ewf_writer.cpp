// EWF (EWF1 / .E01) segment writer — see imager/ewf_writer.h for the layout.
//
// Field layouts follow the EWF specification as implemented by libewf
// (https://github.com/libyal/libewf), the reference open-source reader:
//   file header : 76 bytes, MD5 checksum of bytes [0,72) stored at 0x44
//   section     : 40-byte header (16-byte ASCII type, u64 next offset,
//                 u64 size incl. header, u64 pad)
//   disk/volume : 104 bytes — media flag, 24-bit chunk count, sectors per
//                 chunk, bytes per sector, u64 sector count, zero padding
//   table       : (chunk_count + 1) * u32 — per-chunk byte offset within the
//                 sectors section, then the base (total chunk bytes)
//   digest      : 16 raw MD5 bytes over the image data
#include "imager/ewf_writer.h"

#include <algorithm>
#include <cstring>

namespace wolf {

namespace {
constexpr size_t kFileHeaderSize = 76;
constexpr size_t kSectionHeaderSize = 40;
constexpr size_t kVolumeDataSize = 104;

void putU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xFF);
    v.push_back((x >> 8) & 0xFF);
}
void putU32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}
void putU64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}
} // namespace

EwfWriter::EwfWriter() {}
EwfWriter::~EwfWriter() {
    if (outFile_.is_open()) {
        outFile_.close();
    }
}

void EwfWriter::writeSectionHeader(const char type[16], uint64_t size) {
    uint8_t hdr[kSectionHeaderSize];
    std::memset(hdr, 0, sizeof(hdr));
    std::memcpy(hdr, type, std::min<size_t>(15, std::strlen(type)));
    // next_section_offset / section_size: for a single-segment file both
    // carry the size (the next section starts where this one ends).
    uint64_t v = size;
    for (int i = 0; i < 8; ++i) hdr[16 + i] = (v >> (8 * i)) & 0xFF;
    for (int i = 0; i < 8; ++i) hdr[24 + i] = (v >> (8 * i)) & 0xFF;
    outFile_.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
}

bool EwfWriter::open(const std::string& destPath,
                     uint64_t totalSectors,
                     uint32_t bytesPerSector,
                     const EwfOptions& opts) {
    if (outFile_.is_open()) return false;
    if (bytesPerSector == 0 || opts.sectorsPerChunk == 0) return false;
    // ponytail: single-segment EWF table offsets are uint32. Refuse >4 GiB
    // instead of wrapping currentChunkBytes_ and emitting a corrupt E01.
    if (totalSectors > 0 &&
        totalSectors > (0xffffffffULL / static_cast<uint64_t>(bytesPerSector))) {
        return false;
    }

    outFile_.open(destPath, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!outFile_.is_open()) return false;

    opts_ = opts;
    bytesPerSector_ = bytesPerSector;
    totalSectors_ = totalSectors;
    bytesWritten_ = 0;
    currentChunkBytes_ = 0;
    chunkOffsets_.clear();
    imageMd5_ = crypto::Md5();
    finishedMd5Hex_.clear();

    // ---- File header (76 bytes) ----
    std::vector<uint8_t> fh;
    fh.reserve(kFileHeaderSize);
    static const uint8_t SIG[8] = {'E', 'V', 'F', 0x09, 0x0D, 0x0A, 0xFF, 0x00};
    fh.insert(fh.end(), SIG, SIG + 8);
    putU32(fh, static_cast<uint32_t>(kFileHeaderSize)); // fields start
    putU16(fh, 1); // file format version
    putU16(fh, 1); // backward-compatible version
    putU16(fh, 1); // segment number
    putU16(fh, 1); // total segments
    putU32(fh, 0); // max segment size (single segment)
    putU32(fh, 0); // error granularity
    putU32(fh, 1); // generation
    while (fh.size() < 0x44) fh.push_back(0); // reserved fields
    // Checksum at [0x44, 0x48): first 4 bytes of MD5 over bytes [0, 0x44).
    // Patched in finish(); zero for now.
    putU32(fh, 0);
    // Trailing reserved dword at [0x48, 0x4C) — required to reach 76 bytes.
    putU32(fh, 0);
    outFile_.write(reinterpret_cast<const char*>(fh.data()), fh.size());

    // ---- header section (case metadata) ----
    std::string hd = "\n" + opts_.serial + "\n";
    hd += "c\n1\n" + opts_.caseNumber + "\n";
    hd += "n\n1\n" + opts_.notes + "\n";
    hd += "t\n1\n" + opts_.examiner + "\n";
    hd += "a\n1\nWolf Recovery 0.1\n";
    hd.push_back('\0'); // NUL-terminate for readers scanning to the first zero
    // Pad the data so the total section size is 8-byte aligned.
    size_t hdLen = hd.size();
    size_t padded = (hdLen + 8 + 7) / 8 * 8;
    writeSectionHeader("header", kSectionHeaderSize + padded);
    outFile_.write(hd.data(), hdLen);
    for (size_t i = hdLen; i < padded; ++i) outFile_.put('\0');

    // ---- disk section (volume geometry) ----
    uint64_t chunkSize = static_cast<uint64_t>(opts_.sectorsPerChunk) * bytesPerSector;
    uint64_t chunkCount = (totalSectors_ + opts_.sectorsPerChunk - 1) / opts_.sectorsPerChunk;
    std::vector<uint8_t> vol;
    vol.reserve(kVolumeDataSize);
    vol.push_back(0); // media flag (0 = physical)
    // 24-bit little-endian chunk count
    uint64_t cc = chunkCount;
    for (int i = 0; i < 3; ++i) { vol.push_back(cc & 0xFF); cc >>= 8; }
    putU32(vol, opts_.sectorsPerChunk);
    putU32(vol, bytesPerSector);
    putU64(vol, totalSectors_);
    while (vol.size() < kVolumeDataSize) vol.push_back(0);
    writeSectionHeader("disk", kSectionHeaderSize + kVolumeDataSize);
    outFile_.write(reinterpret_cast<const char*>(vol.data()), vol.size());
    (void)chunkSize;

    // ---- sectors section (all chunk data) ----
    // Section size is unknown until the data has been streamed; write a
    // placeholder header now and patch the sizes in finish().
    sectorsDataStart_ = static_cast<uint64_t>(outFile_.tellp()) + kSectionHeaderSize;
    writeSectionHeader("sectors", 0);
    return true;
}

bool EwfWriter::write(const uint8_t* data, size_t length) {
    if (!outFile_.is_open() || length % bytesPerSector_ != 0) return false;
    if (currentChunkBytes_ > 0xffffffffULL) return false;

    // Track chunk boundaries: a new table entry starts whenever the stream
    // crosses a multiple of the chunk size.
    uint64_t chunkBytes = static_cast<uint64_t>(opts_.sectorsPerChunk) * bytesPerSector_;
    size_t pos = 0;
    while (pos < length) {
        uint64_t before = bytesWritten_;
        if (before % chunkBytes == 0) {
            if (currentChunkBytes_ > 0xffffffffULL) return false;
            chunkOffsets_.push_back(static_cast<uint32_t>(currentChunkBytes_));
        }
        uint64_t spaceInChunk = chunkBytes - (before % chunkBytes);
        size_t take = static_cast<size_t>(std::min<uint64_t>(spaceInChunk, length - pos));
        if (currentChunkBytes_ + take > 0xffffffffULL) return false;
        outFile_.write(reinterpret_cast<const char*>(data + pos), take);
        imageMd5_.update(data + pos, take);
        bytesWritten_ += take;
        currentChunkBytes_ += take;
        pos += take;
    }
    return true;
}

bool EwfWriter::finish() {
    if (!outFile_.is_open()) return false;

    uint64_t sectorsSectionSize = kSectionHeaderSize + currentChunkBytes_;

    // ---- table section ----
    std::vector<uint8_t> table;
    table.reserve((chunkOffsets_.size() + 1) * 4);
    for (uint32_t off : chunkOffsets_) putU32(table, off);
    putU32(table, static_cast<uint32_t>(currentChunkBytes_)); // base offset
    // Pad table data to a multiple of 16 bytes (libewf convention).
    while (table.size() % 16 != 0) table.push_back(0);
    writeSectionHeader("table", kSectionHeaderSize + table.size());
    outFile_.write(reinterpret_cast<const char*>(table.data()), table.size());

    // ---- table2 (backup copy) ----
    writeSectionHeader("table2", kSectionHeaderSize + table.size());
    outFile_.write(reinterpret_cast<const char*>(table.data()), table.size());

    // ---- digest section (raw MD5 of the image data) ----
    uint8_t digest[16];
    imageMd5_.finalRaw(digest);
    {
        char hex[33];
        for (int i = 0; i < 16; ++i) {
            static const char* HEXD = "0123456789abcdef";
            hex[i * 2] = HEXD[digest[i] >> 4];
            hex[i * 2 + 1] = HEXD[digest[i] & 0xF];
        }
        hex[32] = 0;
        finishedMd5Hex_ = hex;
    }
    std::vector<uint8_t> digestData(digest, digest + 16);
    writeSectionHeader("digest", kSectionHeaderSize + digestData.size());
    outFile_.write(reinterpret_cast<const char*>(digestData.data()), digestData.size());

    // ---- done section ----
    writeSectionHeader("done", kSectionHeaderSize);

    // ---- patch the sectors section header with the real size ----
    uint64_t endPos = static_cast<uint64_t>(outFile_.tellp());
    outFile_.seekp(static_cast<std::streamoff>(sectorsDataStart_ - kSectionHeaderSize));
    {
        uint8_t hdr[kSectionHeaderSize];
        std::memset(hdr, 0, sizeof(hdr));
        std::memcpy(hdr, "sectors", 7);
        for (int i = 0; i < 8; ++i) hdr[16 + i] = (sectorsSectionSize >> (8 * i)) & 0xFF;
        for (int i = 0; i < 8; ++i) hdr[24 + i] = (sectorsSectionSize >> (8 * i)) & 0xFF;
        outFile_.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    }

    // ---- patch the file header checksum ----
    // fstream keeps separate read/write pointers, and a read at EOF would
    // trip the fail bit (silently skipping the patch) — position the read
    // pointer explicitly and clear any stale flags first.
    outFile_.clear();
    outFile_.seekg(0);
    std::vector<uint8_t> fh(kFileHeaderSize);
    outFile_.read(reinterpret_cast<char*>(fh.data()), kFileHeaderSize);
    if (!outFile_.fail()) {
        std::string full = crypto::md5Hex(fh.data(), 0x44);
        uint32_t cksum = 0;
        for (int i = 0; i < 4; ++i) {
            unsigned byte = 0;
            char tmp[3] = {full[i * 2], full[i * 2 + 1], 0};
            byte = static_cast<unsigned>(std::strtoul(tmp, nullptr, 16));
            cksum |= byte << (8 * i);
        }
        outFile_.seekp(0x44);
        uint8_t le[4] = {static_cast<uint8_t>(cksum & 0xFF),
                         static_cast<uint8_t>((cksum >> 8) & 0xFF),
                         static_cast<uint8_t>((cksum >> 16) & 0xFF),
                         static_cast<uint8_t>((cksum >> 24) & 0xFF)};
        outFile_.write(reinterpret_cast<const char*>(le), 4);
    }

    outFile_.seekp(static_cast<std::streamoff>(endPos));
    outFile_.close();
    return true;
}

} // namespace wolf
