#include "imager/ewf_reader.h"

#include "crypto/byteback_md5.h"
#include "zlib.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace byteback {

namespace {

constexpr size_t kFileHeaderSize = 76;
constexpr size_t kSectionHeaderSize = 40;
// C3: compression flag byte in the sectors/table section headers (written by
// EwfWriter since the compressed-chunk support; 0 for legacy images).
constexpr size_t kSectionHeaderFlagsOffset = 32;

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

// C3: raw-inflate (windowBits -15) `inLen` bytes into `out` (exactly
// out.size() plaintext bytes expected).
bool inflateChunkRaw(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen) {
    z_stream zs{};
    if (inflateInit2_(&zs, -15, ZLIB_VERSION, static_cast<int>(sizeof(zs))) != Z_OK) {
        return false;
    }
    zs.next_in = const_cast<Bytef*>(in);
    zs.avail_in = static_cast<uInt>(inLen);
    zs.next_out = out;
    zs.avail_out = static_cast<uInt>(outLen);
    const int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    return rc == Z_STREAM_END && zs.avail_out == 0;
}

} // namespace

EwfReader::EwfReader() = default;
EwfReader::~EwfReader() { close(); }

void EwfReader::close() {
    segments_.clear();
    bytesPerSector_ = 0;
    sectorsPerChunk_ = 0;
    imageBytes_ = 0;
    md5Hex_.clear();
    acquiryErrors_.clear();
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
            sectorsPerChunk_ = rdU32(vol.data() + 4);
            imageBytes_ = sectors * bps;
        } else if (std::strcmp(type, "sectors") == 0) {
            out.sectorsDataOffset = off + kSectionHeaderSize;
            out.sectorsDataBytes = secSize - kSectionHeaderSize;
            out.chunkCompression = sh[kSectionHeaderFlagsOffset];
            sawSectors = true;
        } else if (std::strcmp(type, "table") == 0) {
            // C3: chunk byte counts. The writer pads the table to 16 bytes and
            // repeats the section total as the final entry: strip the zero
            // padding, then drop the final entry — the remaining entries are
            // the chunk START offsets (extents end at the next start, the
            // last one at sectorsDataBytes).
            const uint64_t tableBytes = secSize - kSectionHeaderSize;
            if (tableBytes >= 8 && tableBytes % 4 == 0) {
                std::vector<uint8_t> tbl(static_cast<size_t>(tableBytes));
                if (!readExact(src, off + kSectionHeaderSize, tbl.data(), tbl.size(), err)) return false;
                size_t entries = tbl.size() / 4;
                while (entries > 1 && rdU32(tbl.data() + (entries - 1) * 4) == 0) --entries;
                out.chunkTable.clear();
                if (entries >= 2) {
                    out.chunkTable.reserve(entries - 1);
                    for (size_t i = 0; i + 1 < entries; ++i) {
                        out.chunkTable.push_back(rdU32(tbl.data() + i * 4));
                    }
                }
            }
        } else if (std::strcmp(type, "error") == 0) {
            // D2: u32 count, then {u64 startSector, u64 sectorCount} pairs.
            std::vector<uint8_t> ed(static_cast<size_t>(secSize - kSectionHeaderSize));
            if (!readExact(src, off + kSectionHeaderSize, ed.data(), ed.size(), err)) return false;
            if (ed.size() >= 4) {
                const uint32_t count = rdU32(ed.data());
                if (4ull + 16ull * count <= ed.size()) {
                    for (uint32_t i = 0; i < count; ++i) {
                        const uint8_t* e = ed.data() + 4 + 16ull * i;
                        acquiryErrors_.emplace_back(rdU64(e), rdU64(e + 8));
                    }
                }
            }
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
        // The cursor advances by IMAGE bytes, not container bytes: compressed
        // segments contribute their inflated size (chunk count * chunk bytes;
        // a partial final chunk is bounded by imageBytes_ at read time).
        if (sm.chunkCompression) {
            const uint64_t chunkBytes =
                static_cast<uint64_t>(sectorsPerChunk_ ? sectorsPerChunk_ : 128) * bytesPerSector_;
            imageCursor += static_cast<uint64_t>(sm.chunkTable.size()) * chunkBytes;
        } else {
            imageCursor += sm.sectorsDataBytes;
        }
        segments_.push_back(std::move(sm));
    }

    if (imageBytes_ == 0) imageBytes_ = imageCursor;
    return true;
}

// C3: inflate chunk `idx` into seg.cachedData (single-chunk plaintext cache).
bool EwfReader::inflateChunk(SegmentMap& seg, uint64_t idx, std::string& err) const {
    const uint64_t chunkBytes =
        static_cast<uint64_t>(sectorsPerChunk_ ? sectorsPerChunk_ : 128) * bytesPerSector_;
    if (idx >= seg.chunkTable.size()) {
        err = "chunk index out of range";
        return false;
    }
    const uint64_t remaining = imageBytes_ > seg.imageBaseOffset
                                   ? imageBytes_ - seg.imageBaseOffset
                                   : 0;
    const uint64_t offsetInSeg = idx * chunkBytes;
    if (offsetInSeg >= remaining) {
        err = "chunk index past image end";
        return false;
    }
    const uint64_t expectedPlain = std::min<uint64_t>(chunkBytes, remaining - offsetInSeg);

    const uint64_t compOff = seg.chunkTable[static_cast<size_t>(idx)];
    const uint64_t compEnd = idx + 1 < seg.chunkTable.size()
                                 ? seg.chunkTable[static_cast<size_t>(idx + 1)]
                                 : seg.sectorsDataBytes;
    if (compEnd < compOff || compEnd > seg.sectorsDataBytes) {
        err = "invalid chunk extent";
        return false;
    }
    const size_t compLen = static_cast<size_t>(compEnd - compOff);
    std::vector<uint8_t> comp(compLen);
    if (!readExact(*seg.source, seg.sectorsDataOffset + compOff, comp.data(), comp.size(), err)) {
        return false;
    }

    seg.cachedData.assign(static_cast<size_t>(expectedPlain), 0);
    if (!inflateChunkRaw(comp.data(), comp.size(), seg.cachedData.data(), seg.cachedData.size())) {
        err = "chunk decompression failed";
        seg.cachedData.clear();
        seg.cachedChunk = UINT64_MAX;
        return false;
    }
    seg.cachedChunk = idx;
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
        SegmentMap* seg = nullptr;
        for (auto& s : segments_) {
            const uint64_t segImageEnd =
                s.chunkCompression
                    ? std::min<uint64_t>(imageBytes_, s.imageBaseOffset +
                          static_cast<uint64_t>(s.chunkTable.size()) *
                              (sectorsPerChunk_ ? sectorsPerChunk_ : 128) * bytesPerSector_)
                    : s.imageBaseOffset + s.sectorsDataBytes;
            if (pos >= s.imageBaseOffset && pos < segImageEnd) {
                seg = &s;
                break;
            }
        }
        if (!seg) {
            err = "offset outside segment map";
            return false;
        }
        const uint64_t local = pos - seg->imageBaseOffset;

        if (seg->chunkCompression == 0) {
            // Legacy stored-chunk path: the sectors section IS the image.
            const size_t take = static_cast<size_t>(
                std::min<uint64_t>(len - done, seg->sectorsDataBytes - local));
            if (!seg->source->read(seg->sectorsDataOffset + local, buf + done, take)) {
                err = seg->source->lastError();
                return false;
            }
            done += take;
            continue;
        }

        const uint64_t chunkBytes = static_cast<uint64_t>(
            sectorsPerChunk_ ? sectorsPerChunk_ : 128) * bytesPerSector_;
        const uint64_t chunkIdx = local / chunkBytes;
        if (seg->cachedChunk != chunkIdx) {
            if (!inflateChunk(*seg, chunkIdx, err)) return false;
        }
        const uint64_t inChunk = local - chunkIdx * chunkBytes;
        const size_t avail = static_cast<size_t>(seg->cachedData.size() - inChunk);
        const size_t take = std::min<size_t>(len - done, avail);
        std::memcpy(buf + done, seg->cachedData.data() + inChunk, take);
        done += take;
    }
    return true;
}

// CA-039: the digest section was parsed into md5Hex_ but never checked — a
// corrupted or tampered image presented itself as verified. Hash the whole
// image sequentially and compare in constant time.
bool EwfReader::verifyDigest() {
    if (!isOpen() || md5Hex_.empty()) return false;

    crypto::Md5 ctx;
    std::vector<uint8_t> buf(static_cast<size_t>(std::min<uint64_t>(imageBytes_, 1u << 20)));
    if (buf.empty()) return false;

    uint64_t off = 0;
    while (off < imageBytes_) {
        const size_t take = static_cast<size_t>(std::min<uint64_t>(buf.size(), imageBytes_ - off));
        std::string err;
        if (!read(off, buf.data(), take, err)) return false;
        ctx.update(buf.data(), take);
        off += take;
    }

    const std::string got = ctx.finalHex();
    if (got.size() != md5Hex_.size()) return false;
    // Constant-time compare so digest checks do not leak match positions.
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        diff |= static_cast<uint8_t>(got[i] ^ md5Hex_[i]);
    }
    return diff == 0;
}

} // namespace byteback
