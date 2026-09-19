#include "fs/mft_record_view.h"
#include "fs/ntfs_util.h"
#include "byteback_io.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace byteback {
namespace {

bool loadRec0ForRuns(DiskReader& reader, uint64_t partitionOffsetBytes, uint32_t ss,
                     uint32_t bps, uint32_t spc, uint64_t mftLcn, uint32_t recBytes,
                     const uint8_t* boot, std::vector<uint8_t>& rec0, bool* rec0Unread) {
    rec0.assign(recBytes, 0);
    const uint64_t rec0Off = partitionOffsetBytes + mftLcn * static_cast<uint64_t>(spc) * bps;
    auto rec0Res = reader.readSectors(rec0Off, recBytes, rec0.data());
    if (ioUnread(rec0Res, recBytes)) {
        if (rec0Unread) *rec0Unread = true;
        rec0.clear();
        return false;
    }
    if (rec0Unread) *rec0Unread = false;
    ntfs::applyUsaFixup(rec0.data(), recBytes, ss,
                        recBytes >= 8 ? ntfs::u16le(rec0.data() + 4) : 0,
                        recBytes >= 8 ? ntfs::u16le(rec0.data() + 6) : 0);
    if (std::memcmp(rec0.data(), "FILE", 4) == 0) return true;

    if (!boot) return false;
    const uint64_t mirrLcn = ntfs::u64le(boot + 0x38);
    if (mirrLcn == 0 || mirrLcn == mftLcn) return false;
    rec0.assign(recBytes, 0);
    const uint64_t mirrOff = partitionOffsetBytes + mirrLcn * static_cast<uint64_t>(spc) * bps;
    auto mirrRes = reader.readSectors(mirrOff, recBytes, rec0.data());
    if (!readComplete(mirrRes, recBytes)) {
        rec0.clear();
        return false;
    }
    ntfs::applyUsaFixup(rec0.data(), recBytes, ss,
                        recBytes >= 8 ? ntfs::u16le(rec0.data() + 4) : 0,
                        recBytes >= 8 ? ntfs::u16le(rec0.data() + 6) : 0);
    return std::memcmp(rec0.data(), "FILE", 4) == 0;
}

bool readRecordFromRuns(DiskReader& reader, const std::vector<FileRecord::DataRun>& runs,
                        uint32_t ss, uint64_t offset, uint32_t recBytes,
                        std::vector<uint8_t>& rec, uint64_t& byteOff) {
    rec.assign(recBytes, 0);
    byteOff = 0;
    uint64_t skip = offset;
    uint32_t filled = 0;
    bool haveAbs = false;
    for (const auto& run : runs) {
        if (run.startSector == UINT64_MAX) continue;
        const uint64_t runBytes = run.byteCount > 0
            ? run.byteCount
            : run.sectorCount * static_cast<uint64_t>(ss);
        if (skip >= runBytes) {
            skip -= runBytes;
            continue;
        }
        const uint64_t readOff = run.startSector * static_cast<uint64_t>(ss) + skip;
        if (!haveAbs) {
            byteOff = readOff;
            haveAbs = true;
        }
        const uint32_t take = static_cast<uint32_t>(
            std::min<uint64_t>(recBytes - filled, runBytes - skip));
        auto res = reader.readSectors(readOff, take, rec.data() + filled);
        if (!readComplete(res, take)) return false;
        filled += take;
        skip = 0;
        if (filled >= recBytes) return true;
    }
    return filled >= recBytes;
}

} // namespace

std::optional<MftRecordView> getMftRecordView(DiskReader& reader, uint64_t mftRef,
                                              uint64_t partitionOffsetBytes,
                                              bool* unread) {
    if (unread) *unread = false;
    if (!reader.isOpen() && !reader.hasRaidBackend()) return std::nullopt;

    uint32_t ss = reader.getSectorSize();
    if (ss == 0) ss = 512;
    std::vector<uint8_t> boot;
    uint32_t bps = ss;
    uint32_t spc = 8;
    uint64_t mftLcn = 0;
    uint32_t recBytes = 1024;
    bool primaryUnread = false;
    if (!ntfs::loadNtfsBoot(reader, partitionOffsetBytes, 0, ss, boot, bps, spc, mftLcn, recBytes,
                            &primaryUnread)) {
        if (unread && primaryUnread) *unread = true;
        return std::nullopt;
    }

    const uint64_t volumeStartSector = partitionOffsetBytes / ss;
    const uint64_t recByteOff = mftRef * static_cast<uint64_t>(recBytes);
    uint64_t recOff = partitionOffsetBytes
        + mftLcn * static_cast<uint64_t>(spc) * bps
        + recByteOff;
    std::vector<uint8_t> rec(recBytes, 0);
    bool rec0Unread = false;
    std::vector<uint8_t> rec0;
    if (loadRec0ForRuns(reader, partitionOffsetBytes, ss, bps, spc, mftLcn, recBytes,
                        boot.data(), rec0, &rec0Unread) &&
        rec0.size() >= recBytes) {
        auto runs = unnamedDataRunsFromRecord(rec0.data(), recBytes, volumeStartSector, spc);
        if (!runs.empty()) {
            uint64_t mappedOff = 0;
            if (!readRecordFromRuns(reader, runs, ss, recByteOff, recBytes, rec, mappedOff)) {
                if (unread) *unread = true;
                return std::nullopt;
            }
            recOff = mappedOff;
        } else {
            auto recRes = reader.readSectors(recOff, recBytes, rec.data());
            if (ioUnread(recRes, recBytes)) {
                if (unread) *unread = true;
                return std::nullopt;
            }
        }
    } else {
        if (rec0Unread && unread) {
            *unread = true;
            return std::nullopt;
        }
        auto recRes = reader.readSectors(recOff, recBytes, rec.data());
        if (ioUnread(recRes, recBytes)) {
            if (unread) *unread = true;
            return std::nullopt;
        }
    }

    MftRecordView view;
    view.mftRef = mftRef;
    view.byteOffset = recOff;
    view.signature.assign(reinterpret_cast<const char*>(rec.data()), 4);
    if (recBytes >= 0x18) view.flags = ntfs::u16le(rec.data() + 0x16);

    ntfs::applyUsaFixup(rec.data(), recBytes, ss,
                        recBytes >= 8 ? ntfs::u16le(rec.data() + 4) : 0,
                        recBytes >= 8 ? ntfs::u16le(rec.data() + 6) : 0);

    uint32_t attrOff = recBytes >= 0x16 ? ntfs::u16le(rec.data() + 0x14) : 0;
    while (attrOff + 16 <= recBytes && view.attrs.size() < 64) {
        const uint32_t type = ntfs::u32le(rec.data() + attrOff);
        const uint32_t length = ntfs::u32le(rec.data() + attrOff + 4);
        if (type == ntfs::ATTR_END_MARKER || length < 16) break;
        if (attrOff + length > recBytes) break;
        MftAttrSummary a;
        a.type = type;
        a.resident = rec[attrOff + 8] == 0;
        const uint8_t nameLen = rec[attrOff + 9];
        const uint16_t nameOff = ntfs::u16le(rec.data() + attrOff + 10);
        if (nameLen > 0) {
            const size_t namePos = static_cast<size_t>(attrOff) + nameOff;
            if (namePos + static_cast<size_t>(nameLen) * 2 <= attrOff + length &&
                namePos + static_cast<size_t>(nameLen) * 2 <= recBytes) {
                const auto* units = reinterpret_cast<const uint16_t*>(rec.data() + namePos);
                a.name = ntfs::utf16leToUtf8(units, nameLen);
            }
        }
        view.attrs.push_back(std::move(a));
        attrOff += length;
    }
    return view;
}

} // namespace byteback
