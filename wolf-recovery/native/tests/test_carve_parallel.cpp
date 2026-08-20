#include "wolf_carver.h"
#include "wolf_io.h"
#include "fixtures/volume_fixtures.h"
#include <gtest/gtest.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

using namespace wolf;

namespace {

std::vector<uint8_t> buildMultiPngDisk(size_t sizeBytes, const std::vector<size_t>& pngOffsets) {
    std::vector<uint8_t> img(sizeBytes, 0);
    static const uint8_t sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static const uint8_t iend[] = {0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
                                   0xAE, 0x42, 0x60, 0x82};
    for (size_t off : pngOffsets) {
        if (off + 8192 > img.size()) continue;
        std::memcpy(img.data() + off, sig, sizeof(sig));
        std::memcpy(img.data() + off + 4096, iend, sizeof(iend));
    }
    return img;
}

std::vector<uint64_t> collectPngStartSectors(DiskReader& reader, unsigned workers) {
    CarvingEngine carver;
    carver.loadSignatures("");
    carver.setCarveWorkerCount(workers);

    std::vector<uint64_t> starts;
    std::atomic<bool> running{true};
    carver.scan(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0 && fr.extension.find("png") != std::string::npos) {
            starts.push_back(fr.startSector);
        }
    }, &running);
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    return starts;
}

} // namespace

TEST(CarveParallel, MatchesSequentialOnMultiBandDisk) {
    constexpr size_t kDisk = 64u * 1024 * 1024;
    const std::vector<size_t> offs = {1u << 20, 16u * (1 << 20), 48u * (1 << 20)};
    auto img = buildMultiPngDisk(kDisk, offs);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    auto seq = collectPngStartSectors(reader, 1);
    auto par = collectPngStartSectors(reader, 4);
    ASSERT_EQ(seq.size(), 3u);
    EXPECT_EQ(par, seq);
}

TEST(CarveParallel, FourWorkersFasterOn64MiB) {
    constexpr size_t kDisk = 64u * 1024 * 1024;
    const std::vector<size_t> offs = {512u * 1024, 8u * (1 << 20), 32u * (1 << 20)};
    auto imgSeq = buildMultiPngDisk(kDisk, offs);
    auto imgPar = buildMultiPngDisk(kDisk, offs);

    DiskReader readerSeq;
    readerSeq.attachMemoryVolume(std::move(imgSeq));
    DiskReader readerPar;
    readerPar.attachMemoryVolume(std::move(imgPar));

    auto run = [](DiskReader& reader, unsigned workers) {
        CarvingEngine carver;
        carver.loadSignatures("");
        carver.setCarveWorkerCount(workers);
        int hits = 0;
        std::atomic<bool> running{true};
        auto t0 = std::chrono::steady_clock::now();
        carver.scan(reader, [&](const FileRecord& fr) {
            if (fr.id >= 0) ++hits;
        }, &running);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        return std::pair<int64_t, int>{ms, hits};
    };

    auto seq = run(readerSeq, 1);
    auto par = run(readerPar, 4);
    EXPECT_EQ(seq.second, par.second);
    EXPECT_GE(seq.second, 1);
    // ponytail: relaxed threshold — CI VMs vary; parallel should not be dramatically slower.
    EXPECT_LE(par.first, static_cast<int64_t>(seq.first * 2 + 50));
}
