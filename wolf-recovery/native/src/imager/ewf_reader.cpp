#include "imager/ewf_reader.h"

#include <algorithm>
#include <cstring>

namespace wolf {

namespace {

constexpr size_t kFileHeaderSize = 76;
constexpr size_t kSectionHeaderSize = 40;

uint64_t rdU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

uint32_t rdU32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

std::string segmentPathFor(const std::string& destPath, int number) {
    std::string base = destPath;
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

bool readExact(ByteSource& src, uint64_t off, uint8_t* buf, size_t len, std::string& err) {
    if (!src.read(off, buf, len)) {
        err = src.lastError().empty() ? "read failed" : src.lastError();
        return false;
    }
    return true;
}

} // namespace

EwfReader::EwfReader() = default;
EwfReader::~EwfReader() { close(); }

void EwfReader::close() {
    segments_.clear();
    bytesPerSector_ = 0;
    imageBytes_ = 0;
    md5Hex_.clear();
    basePath_.clear();
}

bool EwfReader::open(const std::string& pathOrUrl, std::string& err) {
    close();
    basePath_ = pathOrUrl;
    return parseAllSegments(pathOrUrl, err);
}

bool EwfReader::parseSegment(ByteSource& src, bool firstSegment, SegmentMap& out, std::string& err) {
    std::vector<uint8_t> fh(kFileHeaderSize);
    if (!readExact(src, 0, fh.data(), fh.size(), err)) return false;
    static const uint8_t kSig[8] = {'E', 'V', 'F', 0x09, 0x0D, 0x0A, 0xFF, 0x00};
    if (std::memcmp(fh.data(), kSig, 8) != 0) {
        err = "not an EWF file";
        return false;
    }

    uint64_t off = kFileHeaderSize;
    const uint64_t fileSize = src.size();
    bool sawSectors = false;
    bool sawDigest = false;

    while (off + kSectionHeaderSize <= fileSize) {
        std::vector<uint8_t> sh(kSectionHeaderSize);
        if (!readExact(src, off, sh.data(), sh.size(), err)) return false;
        char type[17] = {0};
        std::memcpy(type, sh.data(), 16);
        const uint64_t secSize = rdU64(sh.data() + 24);
        if (secSize < kSectionHeaderSize || off + secSize > fileSize) {
            err = "invalid section size";
            return false;
        }

        if (std::strcmp(type, "disk") == 0 && firstSegment) {
            std::vector<uint8_t> vol(104);
            if (!readExact(src, off + kSectionHeaderSize, vol.data(), vol.size(), err)) return false;
            const uint32_t bps = rdU32(vol.data() + 8);
            const uint64_t sectors = rdU64(vol.data() + 12);
            if (bps == 0) {
                err = "invalid bytes per sector";
                return false;
            }
            bytesPerSector_ = bps;
            imageBytes_ = sectors * bps;
        } else if (std::strcmp(type, "sectors") == 0) {
            out.sectorsDataOffset = off + kSectionHeaderSize;
            out.sectorsDataBytes = secSize - kSectionHeaderSize;
            sawSectors = true;
        } else if (std::strcmp(type, "digest") == 0) {
            uint8_t digest[16];
            if (!readExact(src, off + kSectionHeaderSize, digest, sizeof(digest), err)) return false;
            char hex[33];
            for (int i = 0; i < 16; ++i) {
                static const char* kHex = "0123456789abcdef";
                hex[i * 2] = kHex[digest[i] >> 4];
                hex[i * 2 + 1] = kHex[digest[i] & 0xF];
            }
            hex[32] = '\0';
            md5Hex_ = hex;
            sawDigest = true;
        } else if (std::strcmp(type, "done") == 0) {
            break;
        }
        off += secSize;
    }

    if (!sawSectors) {
        err = "missing sectors section";
        return false;
    }
    if (firstSegment && bytesPerSector_ == 0) {
        err = "missing disk geometry";
        return false;
    }
    (void)sawDigest;
    return true;
}

bool EwfReader::parseAllSegments(const std::string& firstPath, std::string& err) {
    std::unique_ptr<ByteSource> firstSrc;
    if (isHttpUrl(firstPath)) {
        firstSrc = openHttpByteSource(firstPath, err);
    } else {
        firstSrc = openFileByteSource(firstPath, err);
    }
    if (!firstSrc) return false;

    std::vector<uint8_t> fh(kFileHeaderSize);
    if (!readExact(*firstSrc, 0, fh.data(), fh.size(), err)) return false;
    const uint16_t totalSegs = static_cast<uint16_t>(rdU32(fh.data() + 0x12) & 0xFFFF);
    if (totalSegs == 0) {
        err = "invalid segment count";
        return false;
    }

    uint64_t imageCursor = 0;
    for (uint16_t seg = 1; seg <= totalSegs; ++seg) {
        SegmentMap sm;
        if (seg == 1) {
            sm.source = std::move(firstSrc);
        } else {
            if (isHttpUrl(firstPath)) {
                err = "multi-segment EWF over HTTP not supported";
                return false;
            }
            const std::string path = segmentPathFor(firstPath, seg);
            sm.source = openFileByteSource(path, err);
            if (!sm.source) return false;
        }
        sm.imageBaseOffset = imageCursor;
        if (!parseSegment(*sm.source, seg == 1, sm, err)) return false;
        imageCursor += sm.sectorsDataBytes;
        segments_.push_back(std::move(sm));
    }

    if (imageBytes_ == 0) imageBytes_ = imageCursor;
    return true;
}

bool EwfReader::read(uint64_t offsetBytes, uint8_t* buf, size_t len, std::string& err) {
    if (!isOpen() || !buf) {
        err = "ewf not open";
        return false;
    }
    if (offsetBytes + len > imageBytes_) {
        err = "read past image end";
        return false;
    }

    size_t done = 0;
    while (done < len) {
        const uint64_t pos = offsetBytes + done;
        const SegmentMap* seg = nullptr;
        for (const auto& s : segments_) {
            if (pos >= s.imageBaseOffset && pos < s.imageBaseOffset + s.sectorsDataBytes) {
                seg = &s;
                break;
            }
        }
        if (!seg) {
            err = "offset outside segment map";
            return false;
        }
        const uint64_t local = pos - seg->imageBaseOffset;
        const size_t take = static_cast<size_t>(
            std::min<uint64_t>(len - done, seg->sectorsDataBytes - local));
        if (!seg->source->read(seg->sectorsDataOffset + local, buf + done, take)) {
            err = seg->source->lastError();
            return false;
        }
        done += take;
    }
    return true;
}

} // namespace wolf
