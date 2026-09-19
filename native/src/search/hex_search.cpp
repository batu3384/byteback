#include "search/hex_search.h"
#include "byteback_io.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace byteback {

std::vector<HexSearchHit> searchRawBytes(DiskReader& reader,
                                         const uint8_t* needle, size_t needleLen,
                                         uint64_t maxHits,
                                         std::atomic<bool>* isRunning,
                                         bool* unread) {
    if (unread) *unread = false;
    std::vector<HexSearchHit> out;
    if (!needle || needleLen == 0 || needleLen > kHexSearchMaxNeedle) return out;
    if (!reader.isOpen() && !reader.hasRaidBackend()) return out;

    uint32_t ss = reader.getSectorSize();
    if (ss == 0) ss = 512;
    const uint64_t disk = reader.getDiskSize();
    if (disk < needleLen) return out;
    const uint64_t cap = maxHits == 0 ? kHexSearchMaxHits : std::min(maxHits, kHexSearchMaxHits);

    const size_t overlap = needleLen - 1;
    std::vector<uint8_t> carry(overlap, 0);
    size_t carryN = 0;
    std::vector<uint8_t> buf(kHexSearchChunk);
    uint64_t pos = 0;
    while (pos < disk && out.size() < cap) {
        if (isRunning && !(*isRunning)) break;
        uint32_t want = static_cast<uint32_t>(std::min<uint64_t>(kHexSearchChunk, disk - pos));
        want = (want / ss) * ss;
        if (want == 0) break;
        auto res = reader.readSectors(pos, want, buf.data());
        if (ioUnread(res, want)) {
            if (unread) *unread = true;
            carryN = 0;
            pos += want;
            continue;
        }
        std::vector<uint8_t> hay;
        hay.reserve(carryN + want);
        hay.insert(hay.end(), carry.begin(), carry.begin() + static_cast<std::ptrdiff_t>(carryN));
        hay.insert(hay.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(want));

        const uint8_t* begin = hay.data();
        const uint8_t* end = hay.data() + hay.size();
        const uint8_t* it = begin;
        while (out.size() < cap) {
            it = std::search(it, end, needle, needle + needleLen);
            if (it == end) break;
            out.push_back({pos - carryN + static_cast<uint64_t>(it - begin)});
            ++it;
        }
        if (overlap == 0) {
            carryN = 0;
        } else if (hay.size() >= overlap) {
            carryN = overlap;
            std::memcpy(carry.data(), hay.data() + hay.size() - overlap, overlap);
        } else {
            carryN = 0;
        }
        pos += want;
    }
    return out;
}

} // namespace byteback
