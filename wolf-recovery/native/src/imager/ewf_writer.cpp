// EWF (EWF1 / .E01) multi-segment writer — see imager/ewf_writer.h.
#include "imager/ewf_writer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
    if (outFile_.is_open()) outFile_.close();
}

void EwfWriter::writeSectionHeader(const char type[16], uint64_t size) {
    uint8_t hdr[kSectionHeaderSize];
    std::memset(hdr, 0, sizeof(hdr));
    std::memcpy(hdr, type, std::min<size_t>(15, std::strlen(type)));
    uint64_t v = size;
    for (int i = 0; i < 8; ++i) hdr[16 + i] = (v >> (8 * i)) & 0xFF;
    for (int i = 0; i < 8; ++i) hdr[24 + i] = (v >> (8 * i)) & 0xFF;
    outFile_.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
}

std::string EwfWriter::segmentPathFor(int number) const {
    std::string base = destPath_;
    const size_t slash = base.find_last_of("\\/");
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        base = base.substr(0, dot);
    }
    if (number <= 99) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), ".E%02d", number);
        return base + buf;
    }
    int k = number - 100;
    char a = static_cast<char>('A' + (k / 26));
    char b = static_cast<char>('A' + (k % 26));
    return base + ".E" + a + b;
}

void EwfWriter::patchSegmentFileHeader(const std::string& path, uint16_t number, uint16_t total) {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!f.is_open()) return;
    f.seekp(0x10);
    uint8_t seg[4] = {
        static_cast<uint8_t>(number & 0xFF), static_cast<uint8_t>((number >> 8) & 0xFF),
        static_cast<uint8_t>(total & 0xFF), static_cast<uint8_t>((total >> 8) & 0xFF)
    };
    f.write(reinterpret_cast<const char*>(seg), 4);
    f.flush();
    f.clear();
    f.seekg(0);
    std::vector<uint8_t> fh(kFileHeaderSize);
    f.read(reinterpret_cast<char*>(fh.data()), kFileHeaderSize);
    if (f.fail()) return;
    std::string full = crypto::md5Hex(fh.data(), 0x44);
    uint32_t cksum = 0;
    for (int i = 0; i < 4; ++i) {
        char tmp[3] = {full[i * 2], full[i * 2 + 1], 0};
        cksum |= static_cast<unsigned>(std::strtoul(tmp, nullptr, 16)) << (8 * i);
    }
    f.seekp(0x44);
    uint8_t le[4] = {static_cast<uint8_t>(cksum & 0xFF),
                     static_cast<uint8_t>((cksum >> 8) & 0xFF),
                     static_cast<uint8_t>((cksum >> 16) & 0xFF),
                     static_cast<uint8_t>((cksum >> 24) & 0xFF)};
    f.write(reinterpret_cast<const char*>(le), 4);
}

bool EwfWriter::startSegment(int number, bool first) {
    const std::string path = segmentPathFor(number);
    outFile_.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!outFile_.is_open()) return false;
    segmentNumber_ = number;
    segmentPaths_.push_back(path);
    chunkOffsets_.clear();
    currentChunkBytes_ = 0;

    std::vector<uint8_t> fh;
    fh.reserve(kFileHeaderSize);
    static const uint8_t SIG[8] = {'E', 'V', 'F', 0x09, 0x0D, 0x0A, 0xFF, 0x00};
    fh.insert(fh.end(), SIG, SIG + 8);
    putU32(fh, static_cast<uint32_t>(kFileHeaderSize));
    putU16(fh, 1);
    putU16(fh, 1);
    putU16(fh, static_cast<uint16_t>(number));
    putU16(fh, 1); // total_segments patched in finish()
    putU32(fh, 0);
    putU32(fh, 0);
    putU32(fh, 1);
    while (fh.size() < 0x44) fh.push_back(0);
    putU32(fh, 0);
    putU32(fh, 0);
    outFile_.write(reinterpret_cast<const char*>(fh.data()), fh.size());

    if (first) {
        std::string hd = "\n" + opts_.serial + "\n";
        hd += "c\n1\n" + opts_.caseNumber + "\n";
        hd += "n\n1\n" + opts_.notes + "\n";
        hd += "t\n1\n" + opts_.examiner + "\n";
        hd += "a\n1\nWolf Recovery 0.1\n";
        hd.push_back('\0');
        size_t hdLen = hd.size();
        size_t padded = (hdLen + 8 + 7) / 8 * 8;
        writeSectionHeader("header", kSectionHeaderSize + padded);
        outFile_.write(hd.data(), static_cast<std::streamsize>(hdLen));
        for (size_t i = hdLen; i < padded; ++i) outFile_.put('\0');

        uint64_t chunkCount = (totalSectors_ + opts_.sectorsPerChunk - 1) / opts_.sectorsPerChunk;
        std::vector<uint8_t> vol;
        vol.reserve(kVolumeDataSize);
        vol.push_back(0);
        uint64_t cc = chunkCount;
        for (int i = 0; i < 3; ++i) { vol.push_back(cc & 0xFF); cc >>= 8; }
        putU32(vol, opts_.sectorsPerChunk);
        putU32(vol, bytesPerSector_);
        putU64(vol, totalSectors_);
        while (vol.size() < kVolumeDataSize) vol.push_back(0);
        writeSectionHeader("disk", kSectionHeaderSize + kVolumeDataSize);
        outFile_.write(reinterpret_cast<const char*>(vol.data()), vol.size());
    }

    sectorsDataStart_ = static_cast<uint64_t>(outFile_.tellp()) + kSectionHeaderSize;
    writeSectionHeader("sectors", 0);
    return true;
}

bool EwfWriter::closeSegment(bool last) {
    if (!outFile_.is_open()) return false;

    uint64_t sectorsSectionSize = kSectionHeaderSize + currentChunkBytes_;

    std::vector<uint8_t> table;
    table.reserve((chunkOffsets_.size() + 1) * 4);
    for (uint32_t off : chunkOffsets_) putU32(table, off);
    putU32(table, static_cast<uint32_t>(currentChunkBytes_));
    while (table.size() % 16 != 0) table.push_back(0);
    writeSectionHeader("table", kSectionHeaderSize + table.size());
    outFile_.write(reinterpret_cast<const char*>(table.data()), table.size());
    writeSectionHeader("table2", kSectionHeaderSize + table.size());
    outFile_.write(reinterpret_cast<const char*>(table.data()), table.size());

    if (last) {
        uint8_t digest[16];
        imageMd5_.finalRaw(digest);
        char hex[33];
        for (int i = 0; i < 16; ++i) {
            static const char* HEXD = "0123456789abcdef";
            hex[i * 2] = HEXD[digest[i] >> 4];
            hex[i * 2 + 1] = HEXD[digest[i] & 0xF];
        }
        hex[32] = 0;
        finishedMd5Hex_ = hex;
        writeSectionHeader("digest", kSectionHeaderSize + 16);
        outFile_.write(reinterpret_cast<const char*>(digest), 16);
        writeSectionHeader("done", kSectionHeaderSize);
    }

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
    outFile_.seekp(static_cast<std::streamoff>(endPos));
    outFile_.close();
    return true;
}

bool EwfWriter::rotateSegment() {
    if (!closeSegment(false)) return false;
    return startSegment(segmentNumber_ + 1, false);
}

bool EwfWriter::open(const std::string& destPath,
                     uint64_t totalSectors,
                     uint32_t bytesPerSector,
                     const EwfOptions& opts) {
    if (outFile_.is_open()) return false;
    if (bytesPerSector == 0 || opts.sectorsPerChunk == 0) return false;
    if (maxSectorsSectionBytes_ < 4096) maxSectorsSectionBytes_ = 0xFFFF0000ull;

    destPath_ = destPath;
    opts_ = opts;
    bytesPerSector_ = bytesPerSector;
    totalSectors_ = totalSectors;
    bytesWritten_ = 0;
    currentChunkBytes_ = 0;
    chunkOffsets_.clear();
    segmentPaths_.clear();
    imageMd5_ = crypto::Md5();
    finishedMd5Hex_.clear();
    return startSegment(1, true);
}

bool EwfWriter::write(const uint8_t* data, size_t length) {
    if (!outFile_.is_open() || length % bytesPerSector_ != 0) return false;

    uint64_t chunkBytes = static_cast<uint64_t>(opts_.sectorsPerChunk) * bytesPerSector_;
    size_t pos = 0;
    while (pos < length) {
        uint64_t before = bytesWritten_;
        if (before % chunkBytes == 0) {
            if (currentChunkBytes_ > 0 &&
                currentChunkBytes_ + chunkBytes > maxSectorsSectionBytes_) {
                if (!rotateSegment()) return false;
            }
            if (currentChunkBytes_ > 0xffffffffULL) return false;
            chunkOffsets_.push_back(static_cast<uint32_t>(currentChunkBytes_));
        }
        uint64_t spaceInChunk = chunkBytes - (before % chunkBytes);
        size_t take = static_cast<size_t>(std::min<uint64_t>(spaceInChunk, length - pos));
        if (currentChunkBytes_ + take > 0xffffffffULL) return false;
        outFile_.write(reinterpret_cast<const char*>(data + pos), static_cast<std::streamsize>(take));
        imageMd5_.update(data + pos, take);
        bytesWritten_ += take;
        currentChunkBytes_ += take;
        pos += take;
    }
    return true;
}

bool EwfWriter::finish() {
    if (!outFile_.is_open() && segmentPaths_.empty()) return false;
    if (outFile_.is_open() && !closeSegment(true)) return false;
    const uint16_t total = static_cast<uint16_t>(segmentPaths_.size());
    for (size_t i = 0; i < segmentPaths_.size(); ++i) {
        patchSegmentFileHeader(segmentPaths_[static_cast<size_t>(i)],
                               static_cast<uint16_t>(i + 1), total);
    }
    return true;
}

} // namespace wolf
