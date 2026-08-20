// Pure FAT chain/timestamp math — see fs/fat_chain.h.
#include "fs/fat_chain.h"

namespace byteback {
namespace fat {

int64_t dosTimestampToUnix(uint16_t dosDate, uint16_t dosTime) {
    int year = ((dosDate >> 9) & 0x7F) + 1980;
    int month = (dosDate >> 5) & 0x0F;
    int day = dosDate & 0x1F;
    int hour = (dosTime >> 11) & 0x1F;
    int minute = (dosTime >> 5) & 0x3F;
    int second = (dosTime & 0x1F) * 2;
    if (month < 1 || month > 12 || day < 1 || day > 31) return 0;

    // days from 1970-01-01 (Howard Hinnant's civil-from-days inverse)
    long long y = year;
    y -= (month <= 2);
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;                                            // [0, 399]
    long long doy = (153 * (static_cast<long long>(month) + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                    // [0, 146096]
    long long days = era * 146097 + doe - 719468;
    return days * 86400 + static_cast<long long>(hour) * 3600 +
           static_cast<long long>(minute) * 60 + second;
}

std::vector<ChainRun> chainRuns(FatEntryReader readEntry, int fatBits,
                                uint32_t firstCluster, uint32_t sectorsPerCluster,
                                uint64_t dataStartSector, size_t maxClusters) {
    std::vector<ChainRun> runs;
    if (!readEntry || firstCluster < 2 || sectorsPerCluster == 0) return runs;

    auto isEoc = [fatBits](uint32_t v) {
        if (fatBits == 12) return v >= 0xFF8;
        if (fatBits == 16) return v >= 0xFFF8;
        return v >= 0x0FFFFFF8;
    };
    auto isBad = [fatBits](uint32_t v) {
        if (fatBits == 12) return v == 0xFF7;
        if (fatBits == 16) return v == 0xFFF7;
        return v == 0x0FFFFFF7;
    };

    std::vector<uint32_t> seen;
    uint32_t clus = firstCluster;
    while (clus >= 2 && !isEoc(clus) && runs.size() < maxClusters) {
        for (uint32_t c : seen) {
            if (c == clus) return runs; // cycle guard
        }
        seen.push_back(clus);

        ChainRun run;
        run.startSector = dataStartSector + static_cast<uint64_t>(clus - 2) * sectorsPerCluster;
        run.sectorCount = sectorsPerCluster;
        runs.push_back(run);

        uint32_t next = readEntry(clus);
        if (next < 2 || isBad(next)) break;
        clus = next;
    }
    return runs;
}

} // namespace fat
} // namespace byteback
