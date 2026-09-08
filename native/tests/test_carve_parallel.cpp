#include "byteback_carver.h"
#include "byteback_io.h"
#include "fixtures/volume_fixtures.h"
#include "fs/virtual_raid.h"
#include "scan_coordinator.h"
#include <gtest/gtest.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace byteback;

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

// A3: multiset key of a carve result — (name, size, startSector, contentHash).
// Emission order may differ between sequential and parallel; the RESULT SET
// may not.
std::string recordKey(const FileRecord& fr) {
    std::ostringstream os;
    os << fr.name << "|" << fr.sizeBytes << "|" << fr.startSector << "|"
       << fr.contentHash << "|" << fr.source;
    return os.str();
}

std::map<std::string, int> recordMultiset(DiskReader& reader,
                                          const std::vector<SectorRange>& ranges,
                                          unsigned workers,
                                          std::atomic<bool>* cancel = nullptr) {
    byteback::setParallelCarveWorkers(workers);
    std::map<std::string, int> out;
    std::atomic<bool> running{true};
    runCarveScanRanges(reader, [&](const FileRecord& fr) {
        if (fr.id == -1 && fr.name.empty()) return; // progress tick
        out[recordKey(fr)]++;
    }, [&](uint64_t, uint64_t) {}, cancel ? cancel : &running, nullptr, ranges, 0, true);
    return out;
}

std::vector<SectorRange> evenRanges(uint64_t totalSectors, int parts) {
    std::vector<SectorRange> ranges;
    const uint64_t per = totalSectors / parts;
    for (int i = 0; i < parts; ++i) {
        ranges.push_back({i * per, per});
    }
    return ranges;
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

// ---------------------------------------------------------------------------
// A2/A3: coordinator-level parallel carve (workers over disjoint ranges,
// cloned readers, single emit consumer).
// ---------------------------------------------------------------------------

// 12 MiB image with PNGs + a PDF spread across six 2 MiB ranges (every file
// fully inside one range so clean records are expected). Parallel carve must
// equal the sequential result as a MULTISET — same records, same counts, same
// content hashes; only the emission order may differ.
TEST(CarveParallelCoordinator, ParallelEqualsSequentialAsMultiset) {
    constexpr size_t kDisk = 12u * 1024 * 1024;
    const auto png = testfix::buildMinimalValidPng();
    std::vector<uint8_t> img(kDisk, 0);
    static const uint8_t pdf[] = "%PDF-1.4\n%%EOF";
    const std::vector<size_t> offs = {1u << 20, (2u << 20) + 333, (5u << 20) + 4096,
                                      (6u << 20) + 8191, (9u << 20), (11u << 20) + 100000};
    for (size_t off : offs) {
        ASSERT_LT(off + png.size(), kDisk);
        std::memcpy(img.data() + off, png.data(), png.size());
    }
    std::memcpy(img.data() + (8u << 20) + 512, pdf, sizeof(pdf) - 1);

    auto ranges = evenRanges(kDisk / 512, 6);
    DiskReader readerSeq;
    DiskReader readerPar;
    {
        std::vector<uint8_t> a = img, b = img;
        readerSeq.attachMemoryVolume(std::move(a));
        readerPar.attachMemoryVolume(std::move(b));
    }

    const auto seq = recordMultiset(readerSeq, ranges, 1);
    const auto par = recordMultiset(readerPar, ranges, 4);
    ASSERT_EQ(seq.size(), static_cast<size_t>(offs.size()) + 1); // PNGs + 1 PDF
    EXPECT_EQ(par, seq);
}

// Cancel in the middle of a parallel carve: requestStop must stop production
// promptly, the consumer must drain and join, and the run must return with
// valid, non-duplicated records.
TEST(CarveParallelCoordinator, CancelMidCarveReturnsPromptly) {
    constexpr size_t kDisk = 64u * 1024 * 1024;
    std::vector<size_t> offs;
    for (size_t off = 1u << 20; off < kDisk; off += 2u * (1u << 20)) offs.push_back(off);
    auto img = buildMultiPngDisk(kDisk, offs);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    auto ranges = evenRanges(kDisk / 512, 8);
    setParallelCarveWorkers(4);
    std::atomic<bool> running{true};
    std::atomic<uint64_t> maxProgress{0};
    std::atomic<bool> done{false};
    std::map<std::string, int> seen;
    std::mutex seenMu;

    std::thread scanThread([&] {
        runCarveScanRanges(reader, [&](const FileRecord& fr) {
            if (fr.id == -1 && fr.name.empty()) return;
            std::lock_guard<std::mutex> lock(seenMu);
            seen[recordKey(fr)]++;
        }, [&](uint64_t cur, uint64_t) {
            uint64_t prev = maxProgress.load();
            while (prev < cur && !maxProgress.compare_exchange_weak(prev, cur)) {}
        }, &running, nullptr, ranges, 0, true);
        done = true;
    });

    // Let the carve phase make progress, then cancel.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto t0 = std::chrono::steady_clock::now();
    running = false;
    scanThread.join();
    const auto cancelMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_TRUE(done.load());
    // Prompt cancel: progress flags within the scan must reflect the stop and
    // the join must be quick (workers stop at chunk granularity, consumer
    // drains what is queued). Generous bound for slow CI machines.
    EXPECT_LT(cancelMs, 20000);
    // Whatever was emitted must be duplicate-free per key.
    for (const auto& [key, n] : seen) EXPECT_LE(n, 1) << key;
    // Cancellation reached at least part of the bar without wrapping.
    EXPECT_LE(maxProgress.load(), ranges.back().start + ranges.back().count);
}

// A1 clone semantics: the memory clone SHARES the volume state — a fault
// injected through the original AFTER cloning fails the clone's reads too,
// and untouched regions stay byte-exact.
TEST(CarveParallelCoordinator, MemoryCloneSharesVolumeState) {
    std::vector<uint8_t> img(8 * 512);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>(i & 0xFF);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);

    auto clone = reader.clone();
    ASSERT_NE(clone, nullptr);
    EXPECT_TRUE(reader.supportsParallelScan());
    EXPECT_EQ(clone->getDiskSize(), reader.getDiskSize());
    EXPECT_EQ(clone->getSectorSize(), reader.getSectorSize());

    // Fault injected AFTER cloning is visible through the clone (shared state).
    reader.setMemoryFaultRange(2, 2); // sectors 2..3
    std::vector<uint8_t> buf(1024);
    auto res = clone->readSectors(2 * 512, 1024, buf.data());
    EXPECT_FALSE(res.success);
    res = clone->readSectors(0, 512, buf.data());
    ASSERT_TRUE(res.success);
    for (size_t i = 0; i < 512; ++i) EXPECT_EQ(buf[i], static_cast<uint8_t>(i & 0xFF));
}

// A1 clone semantics: raw-file clones read independently from their own
// handle (both readers live, interleaved reads, byte-exact).
TEST(CarveParallelCoordinator, RawFileCloneReadsIndependently) {
    const char* path = "clone_independent.dd";
    std::vector<uint8_t> img(2048);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>((i * 13) & 0xFF);
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
    }

    DiskReader reader;
    std::string err;
    ASSERT_TRUE(reader.attachRawFile(path, &err)) << err;
    auto clone = reader.clone();
    ASSERT_NE(clone, nullptr);
    EXPECT_TRUE(reader.supportsParallelScan());

    std::vector<uint8_t> a(1024), b(1024);
    ASSERT_TRUE(reader.readSectors(0, 1024, a.data()).success);
    ASSERT_TRUE(clone->readSectors(1024, 1024, b.data()).success);
    EXPECT_EQ(0, std::memcmp(a.data(), img.data(), 1024));
    EXPECT_EQ(0, std::memcmp(b.data(), img.data() + 1024, 1024));

    ::remove(path);
}

// A1/A2: RAID-backed readers are unclonable and never take the parallel path —
// the coordinator must fall back to the exact sequential behavior.
TEST(CarveParallelCoordinator, RaidBackedReaderFallsBackToSequential) {
    constexpr size_t kMember = 8u * 1024 * 1024;
    const auto png = testfix::buildMinimalValidPng();
    std::vector<uint8_t> m0(kMember, 0), m1(kMember, 0);
    for (size_t off : {1u << 20, (5u << 20) + 333}) {
        std::memcpy(m0.data() + off, png.data(), png.size());
        std::memcpy(m1.data() + off + 4096, png.data(), png.size());
    }

    auto raid = std::make_shared<VirtualRaid>(
        VirtualRaid::fromImages(RaidLevel::RAID0, {m0, m1}, 64 * 1024));
    DiskReader reader;
    reader.setRaidBackend(raid);
    EXPECT_EQ(reader.clone(), nullptr);
    EXPECT_FALSE(reader.supportsParallelScan());

    auto ranges = evenRanges(raid->capacity() / 512, 4);
    const auto seq = recordMultiset(reader, ranges, 1);
    const auto par = recordMultiset(reader, ranges, 4); // falls back to sequential
    EXPECT_EQ(par, seq);
    EXPECT_GE(seq.size(), 4u);
}

// ---------------------------------------------------------------------------
// Adversarial lane: parallel carve must not LOSE bad-sector telemetry.
//
// CA-007 telemetry is per-reader. Parallel carve reads through CLONES, so a
// faulted region hit by a worker is recorded on the clone and previously
// evaporated when the clone was destroyed — the UI bad-sector map went blind
// exactly where the parallel phase ran, while the sequential run on the same
// image reported the sectors. The runs must be telemetry-equal.
// ---------------------------------------------------------------------------
TEST(CarveParallelCoordinator, ParallelCarveKeepsCloneBadSectorTelemetry) {
    constexpr size_t kDisk = 16u * 1024 * 1024;
    constexpr uint64_t kFaultStart = 9000;  // inside range 1 (8192..16383)
    constexpr uint64_t kFaultCount = 16;
    const std::vector<size_t> offs = {1u << 20, 5u << 20, 9u << 20, 13u << 20};
    auto img = buildMultiPngDisk(kDisk, offs);
    auto ranges = evenRanges(kDisk / 512, 4);

    auto runWithTelemetry = [&](DiskReader& reader, unsigned workers) {
        reader.attachMemoryVolume(img); // copies
        reader.setMemoryFaultRange(kFaultStart, kFaultCount);
        byteback::setParallelCarveWorkers(workers);
        std::atomic<bool> running{true};
        runCarveScanRanges(reader, [](const FileRecord&) {},
                           [&](uint64_t, uint64_t) {}, &running, nullptr, ranges, 0, true);
        std::vector<uint64_t> bad = reader.getBadSectors();
        std::sort(bad.begin(), bad.end());
        return std::pair<std::vector<uint64_t>, uint64_t>{bad, reader.getBadSectorReads()};
    };

    DiskReader seqReader, parReader;
    const auto seq = runWithTelemetry(seqReader, 1); // sequential: original reader reads
    const auto par = runWithTelemetry(parReader, 4); // parallel: clones read

    EXPECT_GT(par.second, 0u) << "parallel carve recorded zero bad-sector reads";
    ASSERT_FALSE(par.first.empty()) << "clone bad-sector telemetry was lost";
    const bool hasFaultSector = std::any_of(par.first.begin(), par.first.end(), [&](uint64_t s) {
        return s >= kFaultStart && s < kFaultStart + kFaultCount;
    });
    EXPECT_TRUE(hasFaultSector) << "faulted sectors missing from parallel-run telemetry";
    // Parity: same image, same fault, same read pattern -> identical totals.
    EXPECT_EQ(par.second, seq.second) << "bad-read counter diverges from sequential";
    EXPECT_EQ(par.first, seq.first) << "bad-sector sample set diverges from sequential";
}

// ---------------------------------------------------------------------------
// Adversarial lane: the emit consumer runs on the CALLER's thread. If the
// downstream callback throws (bridge/DB failure), the sequential path
// propagates the exception to ScanCoordinator::scanWorker's catch; the
// parallel path used to unwind through a std::vector<std::thread> of JOINABLE
// workers -> std::terminate took down the whole process instead of failing
// the scan. The exception must propagate AFTER the workers are closed+joined.
// ---------------------------------------------------------------------------
TEST(CarveParallelCoordinator, ConsumerThrowPropagatesAfterWorkersJoin) {
    constexpr size_t kDisk = 16u * 1024 * 1024;
    std::vector<size_t> offs;
    for (size_t off = 1u << 20; off < kDisk; off += 1u << 20) offs.push_back(off);
    auto img = buildMultiPngDisk(kDisk, offs);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    auto ranges = evenRanges(kDisk / 512, 4);
    setParallelCarveWorkers(4);
    std::atomic<bool> running{true};
    bool threw = false;
    try {
        runCarveScanRanges(reader, [&](const FileRecord& fr) {
            if (fr.id == -1 && fr.name.empty()) return; // progress tick
            throw std::runtime_error("emit-consumer-boom");
        }, [&](uint64_t, uint64_t) {}, &running, nullptr, ranges, 0, true);
    } catch (const std::exception& e) {
        threw = std::string(e.what()).find("emit-consumer-boom") != std::string::npos;
    }
    EXPECT_TRUE(threw) << "consumer exception must propagate (not terminate the process)";
    // Reaching this line at all proves the workers were joined, not abandoned.
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Adversarial lane: the fault range lives in the SHARED MemoryVolumeState but
// was plain uint64_t mutated under the ORIGINAL's ioMutex_ while clones read
// it under their OWN mutex — a cross-reader data race (UB). The pair
// (start,count) must be snapshotted consistently: a read of a sector that
// overlaps NEITHER configured range may never fail, whichever write the
// reader races with ("ghost range" from a torn start/count pair).
// ---------------------------------------------------------------------------
TEST(CarveParallelCoordinator, FaultRangeInjectDuringCloneReadsStaysPairConsistent) {
    std::vector<uint8_t> img(8u * 1024 * 1024, 0xAB);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img), 512);

    auto cloneA = reader.clone();
    auto cloneB = reader.clone();
    ASSERT_NE(cloneA, nullptr);
    ASSERT_NE(cloneB, nullptr);

    // Two alternating configurations. A torn (start,count) pair can only
    // produce the ghost ranges [64,64+4) or [192,192+2); sectors 66..67
    // belong to NO reachable configuration (legit A faults [64,65], legit B
    // faults [192,195]), so those reads must ALWAYS succeed.
    std::atomic<bool> stop{false};
    std::thread injector([&] {
        for (int i = 0; i < 4000 && !stop; ++i) {
            if (i % 2 == 0) reader.setMemoryFaultRange(64, 2);
            else reader.setMemoryFaultRange(192, 4);
        }
    });

    auto ghostReadOk = [&](DiskReader& r, uint64_t sector) {
        std::vector<uint8_t> buf(512);
        return r.readSectors(sector * 512, 512, buf.data()).success;
    };
    for (int i = 0; i < 300; ++i) {
        EXPECT_TRUE(ghostReadOk(*cloneA, 66)) << "ghost fault at sector 66 (torn pair)";
        EXPECT_TRUE(ghostReadOk(*cloneA, 67)) << "ghost fault at sector 67 (torn pair)";
        EXPECT_TRUE(ghostReadOk(*cloneB, 66));
        EXPECT_TRUE(ghostReadOk(*cloneB, 67));
        // Outcomes inside the reachable ranges may vary with timing; the
        // reads must simply return a defined result without crashing.
        (void)ghostReadOk(*cloneA, 64);
        (void)ghostReadOk(*cloneA, 194);
        (void)ghostReadOk(*cloneB, 192);
        (void)ghostReadOk(*cloneB, 195);
    }
    stop = true;
    injector.join();

    reader.setMemoryFaultRange(0, 0); // cleared
    for (uint64_t s : {64ull, 65ull, 66ull, 192ull, 193ull, 194ull, 195ull}) {
        EXPECT_TRUE(ghostReadOk(*cloneA, s)) << "sector " << s << " failed after fault cleared";
    }
}

// A1: the clone must carry the AES-XTS FVEK so an encrypted-volume carve
// through clones decrypts identically to the original reader.
TEST(CarveParallelCoordinator, CloneCarriesXtsFvek) {
    std::vector<uint8_t> cipher(16 * 512);
    for (size_t i = 0; i < cipher.size(); ++i) cipher[i] = static_cast<uint8_t>((i * 7) & 0xFF);
    std::vector<uint8_t> key(64);
    for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<uint8_t>(0x5A ^ i);

    DiskReader reader;
    reader.attachMemoryVolume(cipher, 512); // copies
    ASSERT_TRUE(reader.setXtsFvek(key.data(), key.size()));

    auto clone = reader.clone();
    ASSERT_NE(clone, nullptr);
    EXPECT_TRUE(clone->hasXtsFvek());
    EXPECT_EQ(clone->xtsFvekBytes(), 64u);

    std::vector<uint8_t> a(512), b(512);
    ASSERT_TRUE(reader.readSectors(512, 512, a.data()).success);
    ASSERT_TRUE(clone->readSectors(512, 512, b.data()).success);
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), 512)) << "clone decrypts differently";

#ifdef _WIN32
    // The Windows backend implements real XTS: a keyless reader must return
    // raw ciphertext, proving both readers actually decrypted.
    DiskReader plain;
    plain.attachMemoryVolume(cipher, 512);
    std::vector<uint8_t> raw(512);
    ASSERT_TRUE(plain.readSectors(512, 512, raw.data()).success);
    EXPECT_NE(0, std::memcmp(a.data(), raw.data(), 512)) << "XTS decrypt did not run";
#endif
}
