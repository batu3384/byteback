#include "fs/ntfs_logfile.h"
#include "fs/ntfs_util.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace wolf {

namespace {

#pragma pack(push, 1)
struct BootSector {
    uint8_t jmp[3];
    char oem[8];
    uint16_t bytesPerSector;
    uint8_t sectorsPerCluster;
    uint8_t reserved[7];
    uint8_t media;
    uint16_t reserved2;
    uint16_t sectorsPerTrack;
    uint16_t numHeads;
    uint32_t hiddenSectors;
    uint32_t reserved3;
    uint64_t totalSectors;
    uint64_t mftCluster;
    uint64_t mftMirrorCluster;
    int8_t clustersPerMftRecord;
    uint8_t reserved4[3];
    int8_t clustersPerIndexRecord;
    uint8_t reserved5[3];
    uint64_t volumeSerial;
    uint32_t checksum;
};

struct MftAttrHeader {
    uint32_t type;
    uint32_t length;
    uint8_t nonResidentFlag;
    uint8_t nameLength;
    uint16_t nameOffset;
    uint16_t flags;
    uint16_t attributeId;
};

struct MftResidentAttr {
    uint32_t valueLength;
    uint16_t valueOffset;
    uint8_t indexedFlag;
    uint8_t padding;
};

struct MftNonResidentAttr {
    uint64_t startingVCN;
    uint64_t lastVCN;
    uint16_t dataRunOffset;
    uint16_t compressionUnit;
    uint32_t padding;
    uint64_t allocatedSize;
    uint64_t realSize;
    uint64_t initializedSize;
};

struct LogRecordHeader {
    uint32_t signature; // 'RCRD'
    uint16_t usaOffset;
    uint16_t usaCount;
    uint64_t lsn;
    uint16_t clientDataLength;
    uint16_t clientDataOffset;
};
#pragma pack(pop)

uint32_t mftRecordSize(int8_t clustersPerRecord, uint32_t sectorSize, uint32_t spc) {
    if (clustersPerRecord < 0) return 1u << static_cast<uint32_t>(-clustersPerRecord);
    uint32_t c = static_cast<uint32_t>(clustersPerRecord);
    return c * spc * sectorSize;
}

std::string utf16LeToUtf8(const uint16_t* data, size_t count) {
    return ntfs::utf16leToUtf8(data, count);
}

bool appendFromRuns(DiskReader& reader, const std::vector<FileRecord::DataRun>& runs,
                    uint32_t sectorSize, std::vector<uint8_t>& out, uint32_t maxBytes) {
    for (const auto& run : runs) {
        if (out.size() >= maxBytes) break;
        if (run.startSector == UINT64_MAX) continue;
        uint64_t runBytes = static_cast<uint64_t>(run.sectorCount) * sectorSize;
        uint64_t want = std::min<uint64_t>(runBytes, maxBytes - out.size());
        if (want == 0) continue;
        size_t before = out.size();
        out.resize(before + static_cast<size_t>(want));
        if (!reader.readSectors(run.startSector * sectorSize,
                                static_cast<uint32_t>(want),
                                out.data() + before).success) {
            out.resize(before);
            continue;
        }
    }
    return !out.empty();
}

bool collectUnnamedData(const uint8_t* mftRec, uint32_t mftSize, DiskReader& reader,
                        uint64_t volumeStartSector, uint32_t sectorSize, uint32_t spc,
                        std::vector<uint8_t>& out, uint32_t maxBytes) {
    if (mftSize < 64) return false;
    uint16_t attrOff = *reinterpret_cast<const uint16_t*>(mftRec + 0x14);
    if (attrOff < 0x38 || attrOff >= mftSize) return false;

    for (uint32_t pos = attrOff; pos + 24 < mftSize; ) {
        auto* attr = reinterpret_cast<const MftAttrHeader*>(mftRec + pos);
        if (attr->type == 0xFFFFFFFF || attr->length < 24) break;
        if (pos + attr->length > mftSize) break;

        if (attr->type == ntfs::ATTR_DATA && attr->nameLength == 0) {
            if (attr->nonResidentFlag == 0) {
                if (pos + sizeof(MftAttrHeader) + sizeof(MftResidentAttr) <= mftSize) {
                    auto* res = reinterpret_cast<const MftResidentAttr*>(mftRec + pos + sizeof(MftAttrHeader));
                    uint32_t vLen = res->valueLength;
                    uint16_t vOff = res->valueOffset;
                    if (pos + vOff + vLen <= mftSize && vLen > 0) {
                        uint32_t take = std::min(vLen, maxBytes);
                        out.assign(mftRec + pos + vOff, mftRec + pos + vOff + take);
                        return true;
                    }
                }
            } else if (pos + sizeof(MftAttrHeader) + sizeof(MftNonResidentAttr) <= mftSize) {
                auto* nonRes = reinterpret_cast<const MftNonResidentAttr*>(mftRec + pos + sizeof(MftAttrHeader));
                uint16_t runOffset = nonRes->dataRunOffset;
                size_t currentRunPos = pos + runOffset;
                int64_t previousLcn = 0;
                std::vector<FileRecord::DataRun> runs;

                while (currentRunPos < pos + attr->length && currentRunPos < mftSize) {
                    uint8_t headerByte = mftRec[currentRunPos];
                    if (headerByte == 0x00) break;
                    uint8_t lenSize = headerByte & 0x0F;
                    uint8_t offSize = (headerByte >> 4) & 0x0F;
                    currentRunPos++;
                    if (currentRunPos + lenSize + offSize > mftSize) break;

                    uint64_t clusterCount = 0;
                    for (int j = 0; j < lenSize; ++j)
                        clusterCount |= static_cast<uint64_t>(mftRec[currentRunPos + j]) << (j * 8);
                    currentRunPos += lenSize;

                    bool sparse = (offSize == 0);
                    int64_t lcnOffset = 0;
                    if (!sparse) {
                        for (int j = 0; j < offSize; ++j)
                            lcnOffset |= static_cast<uint64_t>(mftRec[currentRunPos + j]) << (j * 8);
                        if (mftRec[currentRunPos + offSize - 1] & 0x80) {
                            for (int j = offSize; j < 8; ++j)
                                lcnOffset |= static_cast<int64_t>(0xFF) << (j * 8);
                        }
                        previousLcn += lcnOffset;
                    }
                    currentRunPos += offSize;

                    FileRecord::DataRun run;
                    run.startSector = sparse ? UINT64_MAX
                        : volumeStartSector + static_cast<uint64_t>(previousLcn) * spc;
                    run.sectorCount = clusterCount * spc;
                    runs.push_back(run);
                }
                out.clear();
                return appendFromRuns(reader, runs, sectorSize, out, maxBytes);
            }
        }
        pos += attr->length;
    }
    return false;
}

} // namespace

std::string NtfsLogHintCollector::lowerKey(const std::string& name) {
    std::string out = name;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

void NtfsLogHintCollector::add(const std::string& name, uint64_t mftRef) {
    if (name.size() < 4) return;
    auto key = lowerKey(name);
    auto it = byLowerName_.find(key);
    if (it == byLowerName_.end() || mftRef != UINT64_MAX) {
        byLowerName_[key] = NtfsLogHint{name, mftRef};
    }
    if (mftRef != UINT64_MAX) {
        byMftRef_[mftRef] = name;
    }
}

bool NtfsLogHintCollector::findByName(const std::string& name, uint64_t* mftRefOut) const {
    auto it = byLowerName_.find(lowerKey(name));
    if (it == byLowerName_.end()) return false;
    if (mftRefOut) *mftRefOut = it->second.mftRef;
    return true;
}

bool NtfsLogHintCollector::findByMftRef(uint64_t mftRef, std::string* nameOut) const {
    if (mftRef == UINT64_MAX) return false;
    auto it = byMftRef_.find(mftRef);
    if (it == byMftRef_.end()) return false;
    if (nameOut) *nameOut = it->second;
    return true;
}

namespace {

void emitFilenameHint(const std::string& name, int confidence,
                      FileSystemParser::FileRecordCallback& callback,
                      std::unordered_set<std::string>& seen,
                      NtfsLogHintCollector* collector,
                      uint64_t mftRef) {
    if (name.size() < 4 || seen.count(name)) return;
    if (name.find('.') == std::string::npos) return;
    seen.insert(name);
    if (collector) {
        collector->add(name, mftRef);
        return;
    }

    FileRecord fr;
    fr.id = -1;
    fr.name = name;
    auto slash = name.find_last_of("\\/");
    fr.path = slash != std::string::npos ? name.substr(0, slash) : "/$LogFile";
    fr.sizeBytes = 0;
    fr.status = 3;
    fr.confidence = confidence;
    fr.category = "Document";
    fr.source = "ntfs_logfile";
    callback(fr);
}

void scanUtf16Hints(const uint8_t* data, uint32_t len, int confidence,
                    FileSystemParser::FileRecordCallback& callback,
                    std::unordered_set<std::string>& seen,
                    std::atomic<bool>* isRunning, int maxHints,
                    NtfsLogHintCollector* collector) {
    int hints = 0;
    for (uint32_t i = 0; i + 8 < len && (maxHints == 0 || hints < maxHints); i += 2) {
        if (isRunning && !(*isRunning)) break;
        if (data[i + 1] != 0) continue;
        if (!((data[i] >= 'A' && data[i] <= 'Z') || (data[i] >= 'a' && data[i] <= 'z'))) continue;

        size_t j = i;
        std::vector<uint16_t> chars;
        while (j + 1 < len) {
            uint16_t ch = data[j] | (static_cast<uint16_t>(data[j + 1]) << 8);
            if (ch == 0) break;
            if (ch < 0x20 || ch == 0xFFFD) break;
            chars.push_back(ch);
            j += 2;
            if (chars.size() > 260) break;
        }
        if (chars.size() < 4) continue;
        std::string name = utf16LeToUtf8(chars.data(), chars.size());
        if (seen.count(name)) continue;
        uint64_t mftRef = UINT64_MAX;
        if (i >= 8) {
            uint64_t ref = 0;
            for (int b = 0; b < 8; ++b) ref |= static_cast<uint64_t>(data[i - 8 + b]) << (8 * b);
            ref &= 0x0000FFFFFFFFFFFFULL;
            if (ref > 0 && ref < 0x0FFFFFFFFFFFULL) mftRef = ref;
        }
        emitFilenameHint(name, confidence, callback, seen, collector, mftRef);
        ++hints;
        i = static_cast<uint32_t>(j);
    }
}

void scanRcrdRecords(const uint8_t* data, uint32_t len, int confidence,
                     FileSystemParser::FileRecordCallback& callback,
                     std::unordered_set<std::string>& seen,
                     std::atomic<bool>* isRunning, int maxHints,
                     NtfsLogHintCollector* collector) {
    int hints = 0;
    for (uint32_t i = 0; i + sizeof(LogRecordHeader) < len && (maxHints == 0 || hints < maxHints); ++i) {
        if (isRunning && !(*isRunning)) break;
        if (std::memcmp(data + i, "RCRD", 4) != 0) continue;

        auto* hdr = reinterpret_cast<const LogRecordHeader*>(data + i);
        if (hdr->clientDataLength == 0 || hdr->clientDataOffset < sizeof(LogRecordHeader)) continue;
        if (hdr->clientDataLength > 65535) continue;
        uint32_t clientStart = i + hdr->clientDataOffset;
        uint32_t clientEnd = clientStart + hdr->clientDataLength;
        if (clientEnd > len) continue;

        size_t before = seen.size();
        scanUtf16Hints(data + clientStart, hdr->clientDataLength, confidence,
                       callback, seen, isRunning, maxHints - hints, collector);
        hints += static_cast<int>(seen.size() - before);
        i += std::max<uint32_t>(8, hdr->clientDataOffset + hdr->clientDataLength) - 1;
    }
}

} // namespace

void scanNtfsLogFileHints(DiskReader& reader, uint64_t partitionOffsetBytes,
                          FileSystemParser::FileRecordCallback callback,
                          std::atomic<bool>* isRunning,
                          NtfsLogHintCollector* collector) {
    if (!reader.isOpen() && !reader.hasRaidBackend()) return;
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    const uint64_t volumeStartSector = partitionOffsetBytes / sectorSize;

    std::vector<uint8_t> boot(sectorSize);
    if (!reader.readSectors(partitionOffsetBytes, sectorSize, boot.data()).success) return;
    if (boot.size() < 512 || boot[510] != 0x55 || boot[511] != 0xAA) return;
    if (std::memcmp(boot.data() + 3, "NTFS    ", 8) != 0) return;
    if (boot.size() < sizeof(BootSector)) return;
    auto* bs = reinterpret_cast<BootSector*>(boot.data());

    uint32_t spc = bs->sectorsPerCluster ? bs->sectorsPerCluster : 8;
    uint32_t mftSize = mftRecordSize(bs->clustersPerMftRecord, sectorSize, spc);
    if (mftSize < 1024 || bs->mftCluster == 0) return;
    uint64_t mftOffset = partitionOffsetBytes + bs->mftCluster * spc * sectorSize;
    uint64_t logRecordOffset = mftOffset + 2ull * mftSize;

    std::vector<uint8_t> mftRec(mftSize);
    if (!reader.readSectors(logRecordOffset, mftSize, mftRec.data()).success) return;
    if (std::memcmp(mftRec.data(), "FILE", 4) != 0) return;

    const uint32_t kScanMax = 256u * 1024u * 1024u;
    std::vector<uint8_t> logBuf;
    if (!collectUnnamedData(mftRec.data(), mftSize, reader, volumeStartSector, sectorSize, spc,
                            logBuf, kScanMax)) {
        uint64_t disk = reader.getDiskSize();
        uint64_t off = logRecordOffset + mftSize;
        uint32_t take = 0;
        if (disk > off) take = static_cast<uint32_t>(std::min<uint64_t>(kScanMax, disk - off));
        if (take == 0) return;
        logBuf.resize(take);
        if (!reader.readSectors(off, take, logBuf.data()).success) return;
    }

    if (logBuf.size() >= 4 && std::memcmp(logBuf.data(), "RSTR", 4) == 0) {
        uint64_t lsn = 0;
        if (logBuf.size() >= 16) {
            for (int i = 0; i < 8; ++i) lsn |= static_cast<uint64_t>(logBuf[8 + i]) << (8 * i);
        }
        FileRecord fr;
        fr.id = -1;
        fr.name = "[NTFS] $LogFile restart LSN=" + std::to_string(lsn);
        fr.path = "/$LogFile";
        fr.status = 1;
        fr.confidence = 90;
        fr.category = "System";
        fr.source = "ntfs_logfile_restart";
        callback(fr);
    }

    std::unordered_set<std::string> seen;
    scanRcrdRecords(logBuf.data(), static_cast<uint32_t>(logBuf.size()), 58,
                    callback, seen, isRunning, 0, collector);
    scanUtf16Hints(logBuf.data(), static_cast<uint32_t>(logBuf.size()), 35,
                   callback, seen, isRunning, 0, collector);
}

} // namespace wolf
