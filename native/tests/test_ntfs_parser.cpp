#include "byteback_fs.h"
#include "byteback_io.h"
#include "byteback_recovery.h"
#include "crypto/byteback_md5.h"
#include "fixtures/volume_fixtures.h"
#include "fs/mft_record_view.h"
#include "fs/ntfs_util.h"
#include "test_temp_path.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

using namespace byteback;

namespace {

// Little-endian writers are defined in the fixture-builder block below.
void writeLe16(std::vector<uint8_t>& img, size_t off, uint16_t v);
void writeLe32(std::vector<uint8_t>& img, size_t off, uint32_t v);
void writeLe64(std::vector<uint8_t>& img, size_t off, uint64_t v);

// ---------------------------------------------------------------------------
// FAZ 1.1 (A1): peak-RSS measurement helper. PeakWorkingSetSize is process
// monotonic; gtest_discover_tests runs each TEST in a fresh process, so the
// delta around a single scan isolates that scan's memory. Guarded for
// non-Windows hosts: the suite runs on Windows, posix CI compiles parser
// sources only.
// ---------------------------------------------------------------------------
double PeakWorkingSetMB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
#else
    return 0.0;
#endif
}

// ---------------------------------------------------------------------------
// FAZ 1.1 (A2): synthetic NTFS volume with N MFT records, ~1 KB per record of
// which ~688 B is resident $DATA. Written to a PID-unique file in the temp
// directory and read back through DiskReader::attachRawFile so the fixture
// itself does not live in the measured process's working set. Deleted records
// (flags=0) with resident payload mirror the real-world mix where today's
// tempFiles accumulation (FileRecord + residentData) is largest.
// ---------------------------------------------------------------------------
class NtfsVolumeFile {
public:
    static bool write(uint64_t recordCount, std::string& pathOut, std::string& errOut) {
        constexpr uint32_t ss = 512;
        constexpr uint32_t spc = 8;
        constexpr uint32_t clusterBytes = ss * spc;
        constexpr uint32_t recBytes = 1024;
        const uint64_t mftByteStart = clusterBytes;            // $MFT at LCN 1
        const uint64_t mftZoneBytes = recordCount * recBytes;  // incl. record 0 ($MFT)

        pathOut = (std::filesystem::temp_directory_path() /
                   ("byteback_ntfs_mem_test_" + std::to_string(recordCount) + "_" +
                    std::to_string(hostPid()) + ".img"))
                      .string();
        std::ofstream f(pathOut, std::ios::binary | std::ios::trunc);
        if (!f.good()) {
            errOut = "cannot open " + pathOut;
            return false;
        }

        std::vector<uint8_t> boot(ss, 0);
        std::memcpy(boot.data() + 3, "NTFS    ", 8);
        writeLe16(boot, 0x0B, ss);
        boot[0x0D] = static_cast<uint8_t>(spc);
        writeLe64(boot, 0x30, 1);  // $MFT LCN (spec offset 0x30)
        boot[0x40] = 0xF6;         // 2^-10 -> 1024-byte MFT records (spec offset 0x40)
        boot[510] = 0x55;
        boot[511] = 0xAA;
        f.write(reinterpret_cast<const char*>(boot.data()), static_cast<std::streamsize>(boot.size()));
        std::vector<uint8_t> pad(clusterBytes - ss, 0);
        f.write(reinterpret_cast<const char*>(pad.data()), static_cast<std::streamsize>(pad.size()));

        // One 4 KiB flush buffer = 4 records; keeps fixture RAM flat.
        std::vector<uint8_t> buf(clusterBytes, 0);
        std::vector<uint8_t> rec(recBytes, 0);
        for (uint64_t recNo = 0; recNo < recordCount; ++recNo) {
            buildMftRecord(recNo, recNo == 0, mftZoneBytes, rec);
            const size_t slot = static_cast<size_t>(recNo % (clusterBytes / recBytes));
            std::memcpy(buf.data() + slot * recBytes, rec.data(), recBytes);
            if (slot == clusterBytes / recBytes - 1 || recNo + 1 == recordCount) {
                f.write(reinterpret_cast<const char*>(buf.data()),
                        static_cast<std::streamsize>(buf.size()));
            }
        }
        std::vector<uint8_t> tail(clusterBytes, 0); // run never exceeds the image
        f.write(reinterpret_cast<const char*>(tail.data()),
                static_cast<std::streamsize>(tail.size()));
        if (!f.good()) {
            errOut = "short write to " + pathOut;
            return false;
        }
        return true;
    }

    static void buildMftRecord(uint64_t recNo, bool isMftRecord, uint64_t mftZoneBytes,
                               std::vector<uint8_t>& rec) {
        constexpr uint32_t recBytes = 1024;
        std::fill(rec.begin(), rec.end(), 0);
        std::memcpy(rec.data(), "FILE", 4);
        writeLe16(rec, 0x14, 0x38);  // USA offset
        writeLe16(rec, 0x16, isMftRecord ? 0x01 : 0x00); // in-use flag for $MFT only
        writeLe32(rec, 0x18, 256);   // usedSize
        writeLe32(rec, 0x1C, recBytes);
        size_t attr = 0x38;
        if (!isMftRecord) {
            char name[32];
            std::snprintf(name, sizeof(name), "f%07llu.bin",
                          static_cast<unsigned long long>(recNo));
            const size_t nameLen = std::strlen(name);
            const size_t fnValueLen = 66 + nameLen * 2;
            writeLe32(rec, attr + 0, 0x30);  // $FILE_NAME
            writeLe32(rec, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
            rec[attr + 8] = 0;  // resident
            writeLe32(rec, attr + 16, static_cast<uint32_t>(fnValueLen));
            writeLe16(rec, attr + 20, 24);
            writeLe64(rec, attr + 24, 5);  // parent = root (5)
            writeLe64(rec, attr + 56, 512);
            rec[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
            rec[attr + 24 + 65] = 1;  // name type LONG
            for (size_t i = 0; i < nameLen; ++i)
                writeLe16(rec, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
            attr += 16 + 8 + fnValueLen;
            // ~688 B resident $DATA: today's tempFiles retain this blob per record.
            constexpr uint32_t kResident = 688;
            writeLe32(rec, attr + 0, 0x80);
            writeLe32(rec, attr + 4, 24 + kResident);
            rec[attr + 8] = 0;
            writeLe32(rec, attr + 16, kResident);
            writeLe16(rec, attr + 20, 24);
            for (uint32_t i = 0; i < kResident; ++i)
                rec[attr + 24 + i] = static_cast<uint8_t>((i * 31 + recNo) & 0xFF);
        } else {
            // $MFT record 0: non-resident $DATA, one run covering the zone.
            const uint64_t zoneClusters = (mftZoneBytes + 4095) / 4096;
            uint8_t run[16];
            size_t n = 0;
            int lenBytes = 1;
            for (uint64_t v = zoneClusters; v >= 256; v >>= 8) ++lenBytes;
            run[n++] = static_cast<uint8_t>((2 << 4) | lenBytes); // lenSize, offSize=2
            for (int i = 0; i < lenBytes; ++i)
                run[n++] = static_cast<uint8_t>((zoneClusters >> (8 * i)) & 0xFF);
            run[n++] = 0x01;  // start LCN 1 (LE16)
            run[n++] = 0x00;
            run[n++] = 0x00;  // terminator
            writeLe32(rec, attr + 0, 0x80);
            // 16-byte attr header + 48-byte non-resident header; runs live at
            // dataRunOffset 0x40.
            writeLe32(rec, attr + 4, static_cast<uint32_t>(16 + 0x40 + n));
            rec[attr + 8] = 1;  // non-resident
            writeLe16(rec, attr + 0x20, 0x40);
            writeLe64(rec, attr + 0x30, zoneClusters * 4096);  // realSize
            std::memcpy(rec.data() + attr + 0x40, run, n);
        }
    }

    explicit NtfsVolumeFile(const std::string& path) : path_(path) {}
    ~NtfsVolumeFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    const std::string& path() const { return path_; }

private:
    static uint32_t hostPid() {
#ifdef _WIN32
        return static_cast<uint32_t>(::GetCurrentProcessId());
#else
        return 0;
#endif
    }
    std::string path_;
};

} // namespace

namespace {

void writeLe16(std::vector<uint8_t>& img, size_t off, uint16_t v) {
    img[off] = static_cast<uint8_t>(v & 0xFF);
    img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void writeLe32(std::vector<uint8_t>& img, size_t off, uint32_t v) {
    img[off] = static_cast<uint8_t>(v & 0xFF);
    img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    img[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    img[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void writeLe64(std::vector<uint8_t>& img, size_t off, uint64_t v) {
    for (int i = 0; i < 8; ++i) img[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

std::vector<uint8_t> buildNtfsMftCarveDisk() {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    img[0x0D] = 8;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t mft = 8 * ss;
    std::memcpy(img.data() + mft, "FILE", 4);
    writeLe16(img, mft + 0x14, 0x38);
    writeLe16(img, mft + 0x16, 0x01);
    writeLe32(img, mft + 0x18, 256);
    writeLe32(img, mft + 0x1C, 1024);

    size_t attr = mft + 0x38;
    const char* name = "doc.txt";
    const size_t nameLen = 7;
    const size_t fnValueLen = 66 + nameLen * 2;

    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    writeLe64(img, attr + 56, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i) {
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    }

    attr += 16 + 8 + fnValueLen;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 29);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 5);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, "hello", 5);

    attr += 29;
    writeLe32(img, attr + 0, 0xFFFFFFFF);
    writeLe32(img, attr + 4, 0);
    return img;
}

std::vector<uint8_t> buildNtfsBootMftWalkDisk() {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 1024);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 256);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "doc.txt";
    const size_t nameLen = 7;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    writeLe64(img, attr + 56, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 29);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 5);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, "hello", 5);
    attr += 29;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    const size_t orphan = 40 * ss;
    std::memcpy(img.data() + orphan, "FILE", 4);
    writeLe16(img, orphan + 0x14, 0x38);
    writeLe16(img, orphan + 0x16, 0x01);
    writeLe32(img, orphan + 0x18, 256);
    writeLe32(img, orphan + 0x1C, 1024);
    attr = orphan + 0x38;
    const char* oname = "orphan.bin";
    const size_t onameLen = 10;
    const size_t ofn = 66 + onameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + ofn));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(ofn));
    writeLe16(img, attr + 20, 24);
    img[attr + 24 + 64] = static_cast<uint8_t>(onameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < onameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(oname[i]));
    attr += 16 + 8 + ofn;
    writeLe32(img, attr + 0, 0xFFFFFFFF);
    return img;
}

std::vector<uint8_t> buildNtfsFragmentedMftDisk() {
    constexpr size_t ss = 512;
    constexpr uint32_t spc = 8;
    constexpr size_t clusterBytes = ss * spc;
    constexpr uint32_t mftRecBytes = 1024;
    std::vector<uint8_t> img(ss * 256, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = static_cast<uint8_t>(spc);
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = clusterBytes;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, mftRecBytes);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, clusterBytes * 2);
    writeLe64(img, attr + 0x30, mftRecBytes * 8);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    img[attr + 0x43] = 0x11;
    img[attr + 0x44] = 0x01;
    img[attr + 0x45] = 0x13;
    img[attr + 0x46] = 0x00;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    auto writeMftFile = [&](size_t base, const char* name, uint64_t parent) {
        std::memcpy(img.data() + base, "FILE", 4);
        writeLe16(img, base + 0x14, 0x38);
        writeLe16(img, base + 0x16, 0x01);
        writeLe32(img, base + 0x18, 256);
        writeLe32(img, base + 0x1C, mftRecBytes);
        size_t a = base + 0x38;
        const size_t nameLen = std::strlen(name);
        const size_t fnValueLen = 66 + nameLen * 2;
        writeLe32(img, a + 0, 0x30);
        writeLe32(img, a + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
        writeLe32(img, a + 16, static_cast<uint32_t>(fnValueLen));
        writeLe16(img, a + 20, 24);
        writeLe64(img, a + 24, parent);
        img[a + 24 + 64] = static_cast<uint8_t>(nameLen);
        img[a + 24 + 65] = 1;
        for (size_t i = 0; i < nameLen; ++i)
            writeLe16(img, a + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
        a += 16 + 8 + fnValueLen;
        writeLe32(img, a + 0, 0xFFFFFFFF);
    };

    writeMftFile(rec0 + mftRecBytes, "doc.txt", 5);
    writeMftFile(20 * clusterBytes, "frag.txt", 5);
    return img;
}

std::vector<uint8_t> buildNtfsPathRebuildDisk() {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img = buildNtfsBootMftWalkDisk();
    const size_t rec2 = 8 * ss + 2 * 1024;
    std::memcpy(img.data() + rec2, "FILE", 4);
    writeLe16(img, rec2 + 0x14, 0x38);
    writeLe16(img, rec2 + 0x16, 0x01);
    writeLe32(img, rec2 + 0x18, 256);
    writeLe32(img, rec2 + 0x1C, 1024);
    writeLe16(img, rec2 + 0x1A, 0x02);
    size_t attr = rec2 + 0x38;
    const char* dir = "Users";
    const size_t dirLen = 5;
    const size_t fnLen = 66 + dirLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnLen));
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(dirLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < dirLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(dir[i]));
    attr += 16 + 8 + fnLen;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    const size_t rec3 = rec2 + 1024;
    std::memcpy(img.data() + rec3, "FILE", 4);
    writeLe16(img, rec3 + 0x14, 0x38);
    writeLe16(img, rec3 + 0x16, 0x01);
    writeLe32(img, rec3 + 0x18, 256);
    writeLe32(img, rec3 + 0x1C, 1024);
    attr = rec3 + 0x38;
    const char* file = "bar.jpg";
    const size_t fileLen = 7;
    const size_t ffn = 66 + fileLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + ffn));
    writeLe32(img, attr + 16, static_cast<uint32_t>(ffn));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 2);
    img[attr + 24 + 64] = static_cast<uint8_t>(fileLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < fileLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(file[i]));
    attr += 16 + 8 + ffn;
    writeLe32(img, attr + 0, 0xFFFFFFFF);
    return img;
}

std::vector<uint8_t> buildNtfsMftLogfileBoostDisk() {
    constexpr size_t ss = 512;
    constexpr uint32_t spc = 8;
    constexpr uint32_t mftSize = 1024;
    const size_t mftBase = spc * ss;
    const size_t userMft = mftBase + mftSize;
    const size_t logMft = mftBase + 2 * mftSize;

    std::vector<uint8_t> img(logMft + mftSize, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    img[0x0D] = static_cast<uint8_t>(spc);
    writeLe64(img, 0x30, 1); // $MFT LCN (spec offset)
    img[0x40] = static_cast<uint8_t>(0xF6); // clusters per MFT record (spec offset)
    img[510] = 0x55;
    img[511] = 0xAA;

    // MFT record 1: lost.doc (matches LogFile hint)
    std::memcpy(img.data() + userMft, "FILE", 4);
    writeLe16(img, userMft + 0x14, 0x38);
    writeLe16(img, userMft + 0x16, 0x01);
    writeLe32(img, userMft + 0x18, 256);
    writeLe32(img, userMft + 0x1C, mftSize);
    size_t attr = userMft + 0x38;
    const char* name = "lost.doc";
    const size_t nameLen = 8;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    writeLe64(img, attr + 56, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 29);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 5);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, "hello", 5);
    attr += 29;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    // MFT record 2: $LogFile with RCRD hint for lost.doc
    std::memcpy(img.data() + logMft, "FILE", 4);
    writeLe16(img, logMft + 0x14, 0x38);
    writeLe32(img, logMft + 0x18, 256);
    writeLe32(img, logMft + 0x1C, mftSize);
    std::vector<uint8_t> logPayload(128, 0);
    std::memcpy(logPayload.data(), "RCRD", 4);
    const uint16_t clientOff = 32;
    const uint16_t clientLen = 18;
    writeLe16(logPayload, 0x10, clientLen);
    writeLe16(logPayload, 0x12, clientOff);
    const wchar_t* wname = L"lost.doc";
    for (size_t i = 0; wname[i] != 0; ++i)
        writeLe16(logPayload, clientOff + i * 2, static_cast<uint16_t>(wname[i]));
    attr = logMft + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + logPayload.size()));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(logPayload.size()));
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, logPayload.data(), logPayload.size());
    attr += 16 + 8 + logPayload.size();
    writeLe32(img, attr + 0, 0xFFFFFFFF);
    return img;
}

} // namespace

TEST(NtfsParser, CarvesMftFileRecord) {
    auto img = buildNtfsMftCarveDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<std::string> names;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scan(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0) names.push_back(fr.name);
    }, &running));

    bool found = false;
    for (const auto& n : names) {
        if (n == "doc.txt") found = true;
    }
    EXPECT_TRUE(found);
}

TEST(NtfsParser, ExtractsResidentDataBytes) {
    auto img = buildNtfsMftCarveDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::vector<uint8_t> payload;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scan(reader, [&](const FileRecord& fr) {
        if (fr.name == "doc.txt") payload = fr.residentData;
    }, &running));

    ASSERT_EQ(payload.size(), 5u);
    EXPECT_EQ(std::string(payload.begin(), payload.end()), "hello");
}

TEST(NtfsParser, WalksMftFromBootLcnIgnoresOrphan) {
    auto img = buildNtfsBootMftWalkDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0 && !fr.name.empty()) names.push_back(fr.name);
    }, &running, 0, 0, false));
    bool doc = false, orphan = false;
    for (const auto& n : names) {
        if (n == "doc.txt") doc = true;
        if (n == "orphan.bin") orphan = true;
    }
    EXPECT_TRUE(doc);
    EXPECT_FALSE(orphan);
}

TEST(NtfsParser, BackupBootUsedWhenPrimaryWiped) {
    auto img = buildNtfsBootMftWalkDisk();
    constexpr size_t ss = 512;
    std::memcpy(img.data() + img.size() - ss, img.data(), ss);
    std::memset(img.data() + 3, 0, 8);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0 && !fr.name.empty()) names.push_back(fr.name);
    }, &running, 0, 0, false));
    bool doc = false;
    for (const auto& n : names) {
        if (n == "doc.txt") doc = true;
    }
    EXPECT_TRUE(doc) << "NTFS backup boot at last sector must restore $MFT LCN";
}

TEST(NtfsParser, MftMirrUsedWhenPrimaryRec0Wiped) {
    constexpr size_t ss = 512;
    constexpr uint32_t spc = 8;
    constexpr size_t clusterBytes = ss * spc;
    constexpr uint32_t mirrLcn = 2;
    constexpr uint32_t dataLcn = 300;
    std::vector<uint8_t> img(clusterBytes * 320, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = static_cast<uint8_t>(spc);
    writeLe64(img, 0x30, 1);
    writeLe64(img, 0x38, mirrLcn);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = mirrLcn * clusterBytes;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, clusterBytes);
    writeLe64(img, attr + 0x30, 2048);
    img[attr + 0x40] = 0x12;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = static_cast<uint8_t>(dataLcn & 0xFF);
    img[attr + 0x43] = static_cast<uint8_t>((dataLcn >> 8) & 0xFF);
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = dataLcn * clusterBytes + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 256);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "doc.txt";
    const size_t nameLen = 7;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    writeLe64(img, attr + 56, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 29);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 5);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, "hello", 5);
    attr += 29;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool found = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "doc.txt") found = true;
    }, &running, 0, 0, false));
    EXPECT_TRUE(found) << "$MFTMirr rec0 must restore $MFT data runs when primary rec0 is wiped";
}

TEST(NtfsParser, AttributeListPullsDataFromExtensionRecord) {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 4096);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 400);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "big.dat";
    const size_t nameLen = 7;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;
    writeLe32(img, attr + 0, 0x20);
    writeLe32(img, attr + 4, 56);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 26);
    writeLe16(img, attr + 20, 24);
    writeLe32(img, attr + 24, 0x80);
    writeLe16(img, attr + 28, 26);
    img[attr + 30] = 0;
    img[attr + 31] = 26;
    writeLe64(img, attr + 32, 0);
    writeLe64(img, attr + 40, 3);
    writeLe16(img, attr + 48, 0);
    writeLe32(img, attr + 56, 0xFFFFFFFF);

    const size_t rec3 = rec0 + 3 * 1024;
    std::memcpy(img.data() + rec3, "FILE", 4);
    writeLe16(img, rec3 + 0x14, 0x38);
    writeLe16(img, rec3 + 0x16, 0x01);
    writeLe32(img, rec3 + 0x18, 256);
    writeLe32(img, rec3 + 0x1C, 1024);
    writeLe64(img, rec3 + 0x20, 1);
    attr = rec3 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 29);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 5);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, "hello", 5);
    attr += 29;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool found = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "big.dat" && fr.residentData.size() == 5 &&
            std::memcmp(fr.residentData.data(), "hello", 5) == 0) {
            found = true;
        }
    }, &running, 0, 0, false));
    EXPECT_TRUE(found) << "$ATTRIBUTE_LIST must pull unnamed $DATA from the extension record";
}

TEST(NtfsParser, NonResidentAttributeListPullsDataFromExtensionRecord) {
    constexpr size_t ss = 512;
    constexpr size_t clusterBytes = ss * 8;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, clusterBytes);
    writeLe64(img, attr + 0x30, clusterBytes);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 400);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "big.dat";
    const size_t nameLen = 7;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;
    writeLe32(img, attr + 0, 0x20);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, clusterBytes);
    writeLe64(img, attr + 0x30, 26);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x02;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t listOff = 2 * clusterBytes;
    writeLe32(img, listOff + 0, 0x80);
    writeLe16(img, listOff + 4, 26);
    img[listOff + 6] = 0;
    img[listOff + 7] = 26;
    writeLe64(img, listOff + 8, 0);
    writeLe64(img, listOff + 16, 3);
    writeLe16(img, listOff + 24, 0);

    const size_t rec3 = rec0 + 3 * 1024;
    std::memcpy(img.data() + rec3, "FILE", 4);
    writeLe16(img, rec3 + 0x14, 0x38);
    writeLe16(img, rec3 + 0x16, 0x01);
    writeLe32(img, rec3 + 0x18, 256);
    writeLe32(img, rec3 + 0x1C, 1024);
    writeLe64(img, rec3 + 0x20, 1);
    attr = rec3 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 29);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 5);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, "hello", 5);
    attr += 29;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool found = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "big.dat" && fr.residentData.size() == 5 &&
            std::memcmp(fr.residentData.data(), "hello", 5) == 0) {
            found = true;
        }
    }, &running, 0, 0, false));
    EXPECT_TRUE(found) << "non-resident $ATTRIBUTE_LIST must pull unnamed $DATA from the extension record";
}

TEST(NtfsParser, AttributeListPullsNamedAdsFromExtensionRecord) {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 4096);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 400);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "big.dat";
    const size_t nameLen = 7;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;
    writeLe32(img, attr + 0, 0x20);
    writeLe32(img, attr + 4, 64);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 32);
    writeLe16(img, attr + 20, 24);
    writeLe32(img, attr + 24, 0x80);
    writeLe16(img, attr + 28, 32);
    img[attr + 30] = 1;
    img[attr + 31] = 26;
    writeLe64(img, attr + 32, 0);
    writeLe64(img, attr + 40, 3);
    writeLe16(img, attr + 48, 0);
    writeLe16(img, attr + 50, static_cast<uint16_t>('z'));
    writeLe32(img, attr + 64, 0xFFFFFFFF);

    const size_t rec3 = rec0 + 3 * 1024;
    std::memcpy(img.data() + rec3, "FILE", 4);
    writeLe16(img, rec3 + 0x14, 0x38);
    writeLe16(img, rec3 + 0x16, 0x01);
    writeLe32(img, rec3 + 0x18, 256);
    writeLe32(img, rec3 + 0x1C, 1024);
    writeLe64(img, rec3 + 0x20, 1);
    attr = rec3 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 32);
    img[attr + 8] = 0;
    img[attr + 9] = 1;
    writeLe16(img, attr + 10, 24);
    writeLe32(img, attr + 16, 5);
    writeLe16(img, attr + 20, 26);
    writeLe16(img, attr + 24, static_cast<uint16_t>('z'));
    std::memcpy(img.data() + attr + 26, "hello", 5);
    attr += 32;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool found = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == "ntfs_ads" && fr.name == "big.dat:z" &&
            fr.residentData.size() == 5 &&
            std::memcmp(fr.residentData.data(), "hello", 5) == 0) {
            found = true;
        }
    }, &running, 0, 0, false));
    EXPECT_TRUE(found) << "$ATTRIBUTE_LIST must pull named ADS from the extension record";
}

TEST(NtfsParser, ReparsePointEmitsSymlinkPrintName) {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 4096);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 400);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "link.dat";
    const size_t nameLen = 8;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;

    const char* tgt = "t.txt";
    const size_t tgtChars = 5;
    const size_t tgtBytes = tgtChars * 2;
    const uint32_t rpVal = 8 + 12 + static_cast<uint32_t>(tgtBytes * 2);
    writeLe32(img, attr + 0, 0xC0);
    writeLe32(img, attr + 4, 16 + 8 + rpVal);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, rpVal);
    writeLe16(img, attr + 20, 24);
    writeLe32(img, attr + 24, 0xA000000C);
    writeLe16(img, attr + 28, static_cast<uint16_t>(12 + tgtBytes * 2));
    writeLe16(img, attr + 32, 0);
    writeLe16(img, attr + 34, static_cast<uint16_t>(tgtBytes));
    writeLe16(img, attr + 36, static_cast<uint16_t>(tgtBytes));
    writeLe16(img, attr + 38, static_cast<uint16_t>(tgtBytes));
    writeLe32(img, attr + 40, 0);
    for (size_t i = 0; i < tgtChars; ++i)
        writeLe16(img, attr + 44 + i * 2, static_cast<uint16_t>(tgt[i]));
    for (size_t i = 0; i < tgtChars; ++i)
        writeLe16(img, attr + 44 + tgtBytes + i * 2, static_cast<uint16_t>(tgt[i]));
    attr += 16 + 8 + rpVal;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool found = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "link.dat" && fr.source == "ntfs_reparse" &&
            fr.residentData.size() == 5 &&
            std::memcmp(fr.residentData.data(), "t.txt", 5) == 0) {
            found = true;
        }
    }, &running, 0, 0, false));
    EXPECT_TRUE(found) << "$REPARSE_POINT symlink print name must be recoverable";
}

TEST(NtfsParser, ReparsePointEmitsMountPrintName) {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 4096);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 400);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "mnt.dat";
    const size_t nameLen = 7;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;

    const char* tgt = "D:\\vol";
    const size_t tgtChars = 6;
    const size_t tgtBytes = tgtChars * 2;
    const uint32_t rpVal = 16 + static_cast<uint32_t>(tgtBytes);
    writeLe32(img, attr + 0, 0xC0);
    writeLe32(img, attr + 4, 16 + 8 + rpVal);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, rpVal);
    writeLe16(img, attr + 20, 24);
    writeLe32(img, attr + 24, 0xA0000003);
    writeLe16(img, attr + 28, static_cast<uint16_t>(8 + tgtBytes));
    writeLe16(img, attr + 32, 0);
    writeLe16(img, attr + 34, static_cast<uint16_t>(tgtBytes));
    writeLe16(img, attr + 36, 0);
    writeLe16(img, attr + 38, static_cast<uint16_t>(tgtBytes));
    for (size_t i = 0; i < tgtChars; ++i)
        writeLe16(img, attr + 40 + i * 2, static_cast<uint16_t>(tgt[i]));
    attr += 16 + 8 + rpVal;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool found = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "mnt.dat" && fr.source == "ntfs_reparse" &&
            fr.residentData.size() == 6 &&
            std::memcmp(fr.residentData.data(), "D:\\vol", 6) == 0) {
            found = true;
        }
    }, &running, 0, 0, false));
    EXPECT_TRUE(found) << "$REPARSE_POINT mount 0xA0000003 print name must emit ntfs_reparse";
}

TEST(NtfsParser, NonResidentReparsePointEmitsSymlinkPrintName) {
    constexpr size_t ss = 512;
    constexpr size_t clusterBytes = ss * 8;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, clusterBytes);
    writeLe64(img, attr + 0x30, clusterBytes);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 400);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "link.dat";
    const size_t nameLen = 8;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;

    const char* tgt = "t.txt";
    const size_t tgtChars = 5;
    const size_t tgtBytes = tgtChars * 2;
    writeLe32(img, attr + 0, 0xC0);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, clusterBytes);
    writeLe64(img, attr + 0x30, 8 + 12 + tgtBytes * 2);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x02;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rpOff = 2 * clusterBytes;
    writeLe32(img, rpOff, 0xA000000C);
    writeLe16(img, rpOff + 4, static_cast<uint16_t>(12 + tgtBytes * 2));
    writeLe16(img, rpOff + 8, 0);
    writeLe16(img, rpOff + 10, static_cast<uint16_t>(tgtBytes));
    writeLe16(img, rpOff + 12, static_cast<uint16_t>(tgtBytes));
    writeLe16(img, rpOff + 14, static_cast<uint16_t>(tgtBytes));
    writeLe32(img, rpOff + 16, 0);
    for (size_t i = 0; i < tgtChars; ++i)
        writeLe16(img, rpOff + 20 + i * 2, static_cast<uint16_t>(tgt[i]));
    for (size_t i = 0; i < tgtChars; ++i)
        writeLe16(img, rpOff + 20 + tgtBytes + i * 2, static_cast<uint16_t>(tgt[i]));

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool found = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "link.dat" && fr.source == "ntfs_reparse" &&
            fr.residentData.size() == 5 &&
            std::memcmp(fr.residentData.data(), "t.txt", 5) == 0) {
            found = true;
        }
    }, &running, 0, 0, false));
    EXPECT_TRUE(found) << "non-resident $REPARSE_POINT must emit symlink print name";
}

TEST(NtfsParser, ResidentEaEmitsNamedStream) {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 4096);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 400);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "note.txt";
    const size_t nameLen = 8;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;

    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 32);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 5);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, "hello", 5);
    attr += 32;

    const char* eaName = "user.x";
    const char* eaVal = "bar";
    const uint8_t eaNameLen = 6;
    const uint16_t eaValLen = 3;
    const uint32_t eaPacked = 8 + eaNameLen + 1 + eaValLen; // 18
    const uint32_t eaAligned = (eaPacked + 3u) & ~3u;       // 20
    writeLe32(img, attr + 0, 0xE0);
    writeLe32(img, attr + 4, 16 + 8 + eaAligned);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, eaAligned);
    writeLe16(img, attr + 20, 24);
    writeLe32(img, attr + 24, 0);
    img[attr + 28] = 0;
    img[attr + 29] = eaNameLen;
    writeLe16(img, attr + 30, eaValLen);
    std::memcpy(img.data() + attr + 32, eaName, eaNameLen);
    img[attr + 32 + eaNameLen] = 0;
    std::memcpy(img.data() + attr + 32 + eaNameLen + 1, eaVal, eaValLen);
    attr += 16 + 8 + eaAligned;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool found = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == "ntfs_ea" && fr.name == "note.txt:ea:user.x" &&
            fr.residentData.size() == 3 &&
            std::memcmp(fr.residentData.data(), "bar", 3) == 0) {
            found = true;
        }
    }, &running, 0, 0, false));
    EXPECT_TRUE(found) << "resident $EA must emit as a recoverable named stream";
}

TEST(NtfsParser, AttributeListPullsEaFromExtensionRecord) {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 4096);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 400);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "note.txt";
    const size_t nameLen = 8;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;

    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 32);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 5);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, "hello", 5);
    attr += 32;

    writeLe32(img, attr + 0, 0x20);
    writeLe32(img, attr + 4, 56);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 26);
    writeLe16(img, attr + 20, 24);
    writeLe32(img, attr + 24, 0xE0);
    writeLe16(img, attr + 28, 26);
    img[attr + 30] = 0;
    img[attr + 31] = 26;
    writeLe64(img, attr + 32, 0);
    writeLe64(img, attr + 40, 3);
    writeLe16(img, attr + 48, 0);
    writeLe32(img, attr + 56, 0xFFFFFFFF);

    const size_t rec3 = rec0 + 3 * 1024;
    std::memcpy(img.data() + rec3, "FILE", 4);
    writeLe16(img, rec3 + 0x14, 0x38);
    writeLe16(img, rec3 + 0x16, 0x01);
    writeLe32(img, rec3 + 0x18, 256);
    writeLe32(img, rec3 + 0x1C, 1024);
    writeLe64(img, rec3 + 0x20, 1);
    attr = rec3 + 0x38;
    const char* eaName = "user.x";
    const char* eaVal = "bar";
    const uint8_t eaNameLen = 6;
    const uint16_t eaValLen = 3;
    const uint32_t eaPacked = 8 + eaNameLen + 1 + eaValLen;
    const uint32_t eaAligned = (eaPacked + 3u) & ~3u;
    writeLe32(img, attr + 0, 0xE0);
    writeLe32(img, attr + 4, 16 + 8 + eaAligned);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, eaAligned);
    writeLe16(img, attr + 20, 24);
    writeLe32(img, attr + 24, 0);
    img[attr + 28] = 0;
    img[attr + 29] = eaNameLen;
    writeLe16(img, attr + 30, eaValLen);
    std::memcpy(img.data() + attr + 32, eaName, eaNameLen);
    img[attr + 32 + eaNameLen] = 0;
    std::memcpy(img.data() + attr + 32 + eaNameLen + 1, eaVal, eaValLen);
    attr += 16 + 8 + eaAligned;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool found = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == "ntfs_ea" && fr.name == "note.txt:ea:user.x" &&
            fr.residentData.size() == 3 &&
            std::memcmp(fr.residentData.data(), "bar", 3) == 0) {
            found = true;
        }
    }, &running, 0, 0, false));
    EXPECT_TRUE(found) << "$ATTRIBUTE_LIST must pull $EA from the extension record";
}

TEST(NtfsParser, AttributeListPullsReparseFromExtensionRecord) {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 4096);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 400);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "link.dat";
    const size_t nameLen = 8;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;

    writeLe32(img, attr + 0, 0x20);
    writeLe32(img, attr + 4, 56);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 26);
    writeLe16(img, attr + 20, 24);
    writeLe32(img, attr + 24, 0xC0);
    writeLe16(img, attr + 28, 26);
    img[attr + 30] = 0;
    img[attr + 31] = 26;
    writeLe64(img, attr + 32, 0);
    writeLe64(img, attr + 40, 3);
    writeLe16(img, attr + 48, 0);
    writeLe32(img, attr + 56, 0xFFFFFFFF);

    const size_t rec3 = rec0 + 3 * 1024;
    std::memcpy(img.data() + rec3, "FILE", 4);
    writeLe16(img, rec3 + 0x14, 0x38);
    writeLe16(img, rec3 + 0x16, 0x01);
    writeLe32(img, rec3 + 0x18, 256);
    writeLe32(img, rec3 + 0x1C, 1024);
    writeLe64(img, rec3 + 0x20, 1);
    attr = rec3 + 0x38;
    const char* tgt = "t.txt";
    const size_t tgtChars = 5;
    const size_t tgtBytes = tgtChars * 2;
    const uint32_t rpVal = 8 + 12 + static_cast<uint32_t>(tgtBytes * 2);
    writeLe32(img, attr + 0, 0xC0);
    writeLe32(img, attr + 4, 16 + 8 + rpVal);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, rpVal);
    writeLe16(img, attr + 20, 24);
    writeLe32(img, attr + 24, 0xA000000C);
    writeLe16(img, attr + 28, static_cast<uint16_t>(12 + tgtBytes * 2));
    writeLe16(img, attr + 32, 0);
    writeLe16(img, attr + 34, static_cast<uint16_t>(tgtBytes));
    writeLe16(img, attr + 36, static_cast<uint16_t>(tgtBytes));
    writeLe16(img, attr + 38, static_cast<uint16_t>(tgtBytes));
    writeLe32(img, attr + 40, 0);
    for (size_t i = 0; i < tgtChars; ++i)
        writeLe16(img, attr + 44 + i * 2, static_cast<uint16_t>(tgt[i]));
    for (size_t i = 0; i < tgtChars; ++i)
        writeLe16(img, attr + 44 + tgtBytes + i * 2, static_cast<uint16_t>(tgt[i]));
    attr += 16 + 8 + rpVal;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool found = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "link.dat" && fr.source == "ntfs_reparse" &&
            fr.residentData.size() == 5 &&
            std::memcmp(fr.residentData.data(), "t.txt", 5) == 0) {
            found = true;
        }
    }, &running, 0, 0, false));
    EXPECT_TRUE(found) << "$ATTRIBUTE_LIST must pull $REPARSE_POINT from the extension record";
}

TEST(NtfsParser, NonResidentEaEmitsNamedStream) {
    constexpr size_t ss = 512;
    constexpr size_t clusterBytes = ss * 8;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, clusterBytes);
    writeLe64(img, attr + 0x30, clusterBytes);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 400);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "note.txt";
    const size_t nameLen = 8;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;
    writeLe32(img, attr + 0, 0xE0);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, clusterBytes);
    writeLe64(img, attr + 0x30, 20);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x02;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t eaOff = 2 * clusterBytes;
    const char* eaName = "user.x";
    const char* eaVal = "bar";
    writeLe32(img, eaOff + 0, 0);
    img[eaOff + 4] = 0;
    img[eaOff + 5] = 6;
    writeLe16(img, eaOff + 6, 3);
    std::memcpy(img.data() + eaOff + 8, eaName, 6);
    img[eaOff + 14] = 0;
    std::memcpy(img.data() + eaOff + 15, eaVal, 3);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool found = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == "ntfs_ea" && fr.name == "note.txt:ea:user.x" &&
            fr.residentData.size() == 3 &&
            std::memcmp(fr.residentData.data(), "bar", 3) == 0) {
            found = true;
        }
    }, &running, 0, 0, false));
    EXPECT_TRUE(found) << "non-resident $EA must emit as a recoverable named stream";
}

TEST(NtfsParser, EncryptedStandardInfoIsEfsNotPlaintext) {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 4096);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 400);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    writeLe32(img, attr + 0, 0x10);
    writeLe32(img, attr + 4, 16 + 8 + 48);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 48);
    writeLe16(img, attr + 20, 24);
    writeLe32(img, attr + 24 + 32, 0x4000);
    attr += 16 + 8 + 48;
    const char* name = "secret.txt";
    const size_t nameLen = 10;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 32);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 5);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, "hello", 5);
    attr += 32;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool found = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "secret.txt" && fr.source == "ntfs_efs") found = true;
    }, &running, 0, 0, false));
    EXPECT_TRUE(found) << "FILE_ATTRIBUTE_ENCRYPTED must not look like plaintext ntfs_mft";
}

TEST(NtfsParser, ObjectIdEmitsGuidDiscovery) {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 4096);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 400);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "oid.txt";
    const size_t nameLen = 7;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;

    const uint8_t guid[16] = {
        0x67, 0x45, 0x23, 0x01, 0xAB, 0x89, 0xEF, 0xCD,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    writeLe32(img, attr + 0, 0x40);
    writeLe32(img, attr + 4, 16 + 8 + 16);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 16);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, guid, 16);
    attr += 16 + 8 + 16;

    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 32);
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, 5);
    writeLe16(img, attr + 20, 24);
    std::memcpy(img.data() + attr + 24, "hello", 5);
    attr += 32;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool found = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == "ntfs_object_id" && fr.name == "oid.txt:objectid" &&
            fr.residentData.size() == 36 &&
            std::memcmp(fr.residentData.data(), "01234567-89AB-CDEF-0123-456789ABCDEF", 36) == 0) {
            found = true;
        }
    }, &running, 0, 0, false));
    EXPECT_TRUE(found) << "$OBJECT_ID must emit Windows GUID as ntfs_object_id";
}

TEST(NtfsParser, VolumeNameEmitsLabelDiscovery) {
    constexpr size_t ss = 512;
    std::vector<uint8_t> img(ss * 64, 0);
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    writeLe16(img, 0x0B, 512);
    img[0x0D] = 8;
    writeLe64(img, 0x30, 1);
    img[0x40] = 0xF6;
    img[510] = 0x55;
    img[511] = 0xAA;

    const size_t rec0 = 8 * ss;
    std::memcpy(img.data() + rec0, "FILE", 4);
    writeLe16(img, rec0 + 0x14, 0x38);
    writeLe16(img, rec0 + 0x16, 0x01);
    writeLe32(img, rec0 + 0x18, 256);
    writeLe32(img, rec0 + 0x1C, 1024);
    size_t attr = rec0 + 0x38;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 72);
    img[attr + 8] = 1;
    writeLe16(img, attr + 0x20, 0x40);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 4096);
    img[attr + 0x40] = 0x11;
    img[attr + 0x41] = 0x01;
    img[attr + 0x42] = 0x01;
    writeLe32(img, attr + 72, 0xFFFFFFFF);

    const size_t rec1 = rec0 + 1024;
    std::memcpy(img.data() + rec1, "FILE", 4);
    writeLe16(img, rec1 + 0x14, 0x38);
    writeLe16(img, rec1 + 0x16, 0x01);
    writeLe32(img, rec1 + 0x18, 400);
    writeLe32(img, rec1 + 0x1C, 1024);
    attr = rec1 + 0x38;
    const char* name = "$Volume";
    const size_t nameLen = 7;
    const size_t fnValueLen = 66 + nameLen * 2;
    writeLe32(img, attr + 0, 0x30);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + fnValueLen));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(fnValueLen));
    writeLe16(img, attr + 20, 24);
    writeLe64(img, attr + 24, 5);
    img[attr + 24 + 64] = static_cast<uint8_t>(nameLen);
    img[attr + 24 + 65] = 1;
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, attr + 24 + 66 + i * 2, static_cast<uint16_t>(name[i]));
    attr += 16 + 8 + fnValueLen;

    const char* label = "BYTEBACK";
    const size_t labelLen = 8;
    writeLe32(img, attr + 0, 0x60);
    writeLe32(img, attr + 4, static_cast<uint32_t>(16 + 8 + labelLen * 2));
    img[attr + 8] = 0;
    writeLe32(img, attr + 16, static_cast<uint32_t>(labelLen * 2));
    writeLe16(img, attr + 20, 24);
    for (size_t i = 0; i < labelLen; ++i)
        writeLe16(img, attr + 24 + i * 2, static_cast<uint16_t>(label[i]));
    attr += 16 + 8 + labelLen * 2;
    writeLe32(img, attr + 0, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool found = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == "ntfs_vol_name" && fr.name == "BYTEBACK") found = true;
    }, &running, 0, 0, false));
    EXPECT_TRUE(found) << "$VOLUME_NAME must emit the volume label as ntfs_vol_name";
}

TEST(NtfsParser, BadMftSectorDoesNotDropSiblingRecords) {
    auto img = buildNtfsBootMftWalkDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    // rec1 (doc.txt) sits at byte 5120 = sectors 10-11. A 4 MiB chunk used
    // to fail entirely on that overlap and drop rec0 as well.
    reader.setMemoryFaultRange(10, 2);

    bool sawDoc = false;
    bool sawUnread = false;
    int named = 0;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == kNtfsMftUnreadSource) sawUnread = true;
        if (fr.id >= 0 && !fr.name.empty()) {
            ++named;
            if (fr.name == "doc.txt") sawDoc = true;
        }
    }, &running, 0, 0, false));
    EXPECT_FALSE(sawDoc);
    EXPECT_TRUE(sawUnread);
    EXPECT_GE(named, 1);
}

TEST(NtfsParser, UnreadBootIsNotEmptyVolume) {
    auto img = buildNtfsBootMftWalkDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    reader.setMemoryFaultRange(0, 1);

    bool sawDoc = false;
    bool sawUnread = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == kNtfsMftUnreadSource) sawUnread = true;
        if (fr.name == "doc.txt") sawDoc = true;
    }, &running, 0, 0, false));
    EXPECT_FALSE(sawDoc);
    EXPECT_TRUE(sawUnread);
}

TEST(NtfsParser, CarveOrphansFindsFileOutsideMftRuns) {
    auto img = buildNtfsBootMftWalkDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0 && !fr.name.empty()) names.push_back(fr.name);
    }, &running, 0, 0, true));
    bool orphan = false;
    for (const auto& n : names) {
        if (n == "orphan.bin") orphan = true;
    }
    EXPECT_TRUE(orphan);
}

TEST(NtfsParser, FragmentedMftExtentFindsSecondRun) {
    auto img = buildNtfsFragmentedMftDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0 && !fr.name.empty()) names.push_back(fr.name);
    }, &running, 0, 0, false));
    bool doc = false, frag = false;
    for (const auto& n : names) {
        if (n == "doc.txt") doc = true;
        if (n == "frag.txt") frag = true;
    }
    EXPECT_TRUE(doc);
    EXPECT_TRUE(frag);
}

TEST(NtfsParser, RebuildsDeletedFilePath) {
    auto img = buildNtfsPathRebuildDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::string path;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "bar.jpg") path = fr.path;
    }, &running, 0, 0, false));
    EXPECT_EQ(path, "/Users/bar.jpg");
}

TEST(NtfsParser, LogfileHintBoostsMatchingMftRecord) {
    auto img = buildNtfsMftLogfileBoostDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    FileRecord boosted{};
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scan(reader, [&](const FileRecord& fr) {
        if (fr.name == "lost.doc") boosted = fr;
    }, &running));
    EXPECT_EQ(boosted.source, "ntfs_mft_logfile");
    EXPECT_GE(boosted.confidence, 12);
}

TEST(NtfsParser, IndexRootSlackSurvivesMftReuse) {
    auto img = byteback::testfix::buildNtfsIndexRootReuseVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::vector<FileRecord> hits;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0 && !fr.name.empty()) hits.push_back(fr);
    }, &running, 0, 0, false));

    bool alive = false, gone = false, goneIsI30 = false;
    for (const auto& fr : hits) {
        if (fr.name == "alive.bin" && fr.source == "ntfs_mft") alive = true;
        if (fr.name == "gone.txt") {
            gone = true;
            goneIsI30 = fr.source == "ntfs_i30" && fr.status == 0;
        }
    }
    EXPECT_TRUE(alive);
    EXPECT_TRUE(gone);
    EXPECT_TRUE(goneIsI30);
}

TEST(NtfsParser, IndexAllocationNameEmitted) {
    auto img = byteback::testfix::buildNtfsIndexAllocationVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool hit = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "indx_only.txt") {
            hit = fr.source == "ntfs_i30" && fr.status == 0;
        }
    }, &running, 0, 0, false));
    EXPECT_TRUE(hit);
}

TEST(NtfsParser, UnreadIndexAllocationIsSentinelNotSilentMiss) {
    auto img = byteback::testfix::buildNtfsIndexAllocationVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    // $I30 INDX at cluster 3, 8 sectors/cluster, 512 B → sectors 24-31.
    reader.setMemoryFaultRange(24, 8);

    bool sawName = false;
    bool sawUnread = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "indx_only.txt") sawName = true;
        if (fr.source == "ntfs_i30_unread") sawUnread = true;
    }, &running, 0, 0, false));
    EXPECT_FALSE(sawName) << "unread $I30 must not parse zeros as a missing indx_only.txt";
    EXPECT_TRUE(sawUnread);
}

TEST(NtfsParser, OrphanIndxMagicDoesNotEmitI30) {
    auto img = buildNtfsBootMftWalkDisk();
    const size_t indx = 50 * 512;
    std::memcpy(img.data() + indx, "INDX", 4);
    img[indx + 0x18] = 16;
    img[indx + 0x1C] = 16;
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool i30 = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == "ntfs_i30") i30 = true;
    }, &running, 0, 0, true));
    EXPECT_FALSE(i30);
}

TEST(NtfsParser, UnallocIndxEmitsNameFromFreeCluster) {
    auto img = byteback::testfix::buildNtfsUnallocIndxVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    FileRecord unalloc{};
    bool fromMftI30 = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "unalloc_only.txt") unalloc = fr;
        if (fr.name == "indx_only.txt" && fr.source == "ntfs_i30") fromMftI30 = true;
    }, &running, 0, 0, false));
    EXPECT_EQ(unalloc.source, "ntfs_i30_unalloc");
    EXPECT_EQ(unalloc.status, 0);
    EXPECT_NE(unalloc.path.find("unalloc_only.txt"), std::string::npos)
        << "unalloc INDX recarve must keep the FILE_NAME in the rebuilt path";
    EXPECT_TRUE(fromMftI30);
}

TEST(NtfsParser, UnallocIndxUnreadOnIoFail) {
    auto img = byteback::testfix::buildNtfsUnallocIndxVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    reader.setMemoryFaultRange(80, 8);

    bool hit = false;
    bool sawUnread = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "unalloc_only.txt") hit = true;
        if (fr.source == "ntfs_i30_unread") sawUnread = true;
    }, &running, 0, 0, false));
    EXPECT_FALSE(hit) << "unread unalloc INDX must not parse as unalloc_only.txt";
    EXPECT_TRUE(sawUnread);
}

TEST(NtfsParser, UnallocIndxSkipsLiveMftSameName) {
    auto img = byteback::testfix::buildNtfsIndexAllocationVolume();
    img.resize(512 * 128, 0);
    auto indx = byteback::testfix::buildIndxNamed("Dir", 1);
    std::memcpy(img.data() + 10 * 4096, indx.data(), indx.size());
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    bool unallocDir = false;
    bool liveDir = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "Dir" && fr.source == "ntfs_i30_unalloc") unallocDir = true;
        if (fr.name == "Dir" && fr.source == "ntfs_mft") liveDir = true;
    }, &running, 0, 0, false));
    EXPECT_TRUE(liveDir);
    EXPECT_FALSE(unallocDir) << "live MFT Dir must not also emit as unalloc INDX Extra Found";
}

TEST(NtfsParser, UnallocIndxCarveBindJpeg) {
    const auto jpeg = byteback::testfix::minimalValidJpeg(0xAB);
    auto img = byteback::testfix::buildNtfsUnallocIndxJpegVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    bool nameOnly = false;
    FileRecord carve{};
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "gone.jpg" && fr.source == "ntfs_i30_unalloc" && fr.status == 0)
            nameOnly = true;
        if (fr.name == "gone.jpg" && fr.source == "ntfs_i30_carve")
            carve = fr;
    }, &running, 0, 0, false));
    EXPECT_TRUE(nameOnly) << "name-only Extra Found must stay";
    ASSERT_FALSE(carve.runs.empty());
    EXPECT_EQ(carve.sizeBytes, jpeg.size());
    EXPECT_EQ(carve.status, 0);

    const auto dest = bytebackTestTemp("byteback_i30_carve").string();
    std::filesystem::create_directories(dest);
    RecoveryEngine engine;
    auto result = engine.recoverFile(reader, carve, dest);
    EXPECT_TRUE(result.success) << result.error;
    crypto::Md5 md5;
    md5.update(jpeg.data(), jpeg.size());
    EXPECT_EQ(result.md5Hash, md5.finalHex());
    std::error_code ec;
    std::filesystem::remove_all(dest, ec);
}

TEST(NtfsParser, UnallocIndxCarveDoesNotStealLiveData) {
    auto img = byteback::testfix::buildNtfsUnallocIndxJpegVolume(true);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    bool nameOnly = false;
    bool carve = false;
    bool liveData = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "gone.jpg" && fr.source == "ntfs_i30_unalloc") nameOnly = true;
        if (fr.source == "ntfs_i30_carve") carve = true;
        if (fr.source == "ntfs_mft" && !fr.runs.empty() && fr.sizeBytes > 0) liveData = true;
    }, &running, 0, 0, false));
    EXPECT_TRUE(nameOnly);
    EXPECT_TRUE(liveData);
    EXPECT_FALSE(carve) << "live MFT $DATA must not become ntfs_i30_carve";
}

TEST(NtfsParser, UnallocIndxCarveBindPng) {
    const auto png = byteback::testfix::buildMinimalValidPng();
    auto img = byteback::testfix::buildNtfsUnallocIndxPngVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    bool nameOnly = false;
    FileRecord carve{};
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "gone.png" && fr.source == "ntfs_i30_unalloc" && fr.status == 0)
            nameOnly = true;
        if (fr.name == "gone.png" && fr.source == "ntfs_i30_carve")
            carve = fr;
    }, &running, 0, 0, false));
    EXPECT_TRUE(nameOnly);
    ASSERT_FALSE(carve.runs.empty());
    EXPECT_EQ(carve.sizeBytes, png.size());

    const auto dest = bytebackTestTemp("byteback_i30_png").string();
    std::filesystem::create_directories(dest);
    RecoveryEngine engine;
    auto result = engine.recoverFile(reader, carve, dest);
    EXPECT_TRUE(result.success) << result.error;
    crypto::Md5 md5;
    md5.update(png.data(), png.size());
    EXPECT_EQ(result.md5Hash, md5.finalHex());
    std::error_code ec;
    std::filesystem::remove_all(dest, ec);
}

TEST(NtfsParser, UnallocIndxCarveBindPdf) {
    const auto pdf = byteback::testfix::buildMinimalPdf();
    auto img = byteback::testfix::buildNtfsUnallocIndxPdfVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    bool nameOnly = false;
    FileRecord carve{};
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "gone.pdf" && fr.source == "ntfs_i30_unalloc" && fr.status == 0)
            nameOnly = true;
        if (fr.name == "gone.pdf" && fr.source == "ntfs_i30_carve")
            carve = fr;
    }, &running, 0, 0, false));
    EXPECT_TRUE(nameOnly);
    ASSERT_FALSE(carve.runs.empty());
    EXPECT_EQ(carve.sizeBytes, pdf.size());

    const auto dest = bytebackTestTemp("byteback_i30_pdf").string();
    std::filesystem::create_directories(dest);
    RecoveryEngine engine;
    auto result = engine.recoverFile(reader, carve, dest);
    EXPECT_TRUE(result.success) << result.error;
    crypto::Md5 md5;
    md5.update(pdf.data(), pdf.size());
    EXPECT_EQ(result.md5Hash, md5.finalHex());
    std::error_code ec;
    std::filesystem::remove_all(dest, ec);
}

TEST(NtfsParser, UnallocIndxCarveBindZip) {
    const auto zip = byteback::testfix::buildMinimalZip();
    auto img = byteback::testfix::buildNtfsUnallocIndxZipVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    bool nameOnly = false;
    FileRecord carve{};
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "gone.zip" && fr.source == "ntfs_i30_unalloc" && fr.status == 0)
            nameOnly = true;
        if (fr.name == "gone.zip" && fr.source == "ntfs_i30_carve")
            carve = fr;
    }, &running, 0, 0, false));
    EXPECT_TRUE(nameOnly);
    ASSERT_FALSE(carve.runs.empty());
    EXPECT_EQ(carve.sizeBytes, zip.size());

    const auto dest = bytebackTestTemp("byteback_i30_zip").string();
    std::filesystem::create_directories(dest);
    RecoveryEngine engine;
    auto result = engine.recoverFile(reader, carve, dest);
    EXPECT_TRUE(result.success) << result.error;
    crypto::Md5 md5;
    md5.update(zip.data(), zip.size());
    EXPECT_EQ(result.md5Hash, md5.finalHex());
    std::error_code ec;
    std::filesystem::remove_all(dest, ec);
}

TEST(NtfsParser, AdsSurvivesParentDedup) {
    auto img = byteback::testfix::buildNtfsDeletedResidentVolume();
    const size_t rec1 = 8 * 512 + 1024;
    const size_t fnValueLen = 66 + 7 * 2;
    size_t attr = rec1 + 0x38 + 16 + 8 + fnValueLen + 29;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 40);
    img[attr + 8] = 0;
    img[attr + 9] = 1;
    writeLe16(img, attr + 10, 24);
    writeLe32(img, attr + 16, 1);
    writeLe16(img, attr + 20, 26);
    img[attr + 24] = 'Z';
    img[attr + 25] = 0;
    img[attr + 26] = 'x';
    writeLe32(img, attr + 40, 0xFFFFFFFF);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool parent = false, ads = false;
    int adsConfidence = -1;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "doc.txt") { parent = true; }
        if (fr.source == "ntfs_ads" && fr.name.find(":Z") != std::string::npos) {
            ads = true;
            adsConfidence = fr.confidence;  // legacy: raw parent score (50) - 5
        }
    }, &running, 0, 0, false));
    EXPECT_TRUE(parent);
    EXPECT_TRUE(ads);
    EXPECT_EQ(adsConfidence, 45);
}

std::vector<uint8_t> buildNtfsUsnJournalDisk() {
    auto img = buildNtfsBootMftWalkDisk();
    const size_t rec1 = 8 * 512 + 1024;
    const size_t fnValueLen = 66 + 7 * 2;
    size_t attr = rec1 + 0x38 + 16 + 8 + fnValueLen + 29;
    writeLe32(img, attr + 0, 0x80);
    writeLe32(img, attr + 4, 0x48);
    img[attr + 8] = 1;
    img[attr + 9] = 2;
    writeLe16(img, attr + 10, 0x40);
    writeLe16(img, attr + 0x20, 0x44);
    writeLe64(img, attr + 0x28, 4096);
    writeLe64(img, attr + 0x30, 80);
    writeLe16(img, attr + 0x40, '$');
    writeLe16(img, attr + 0x42, 'J');
    img[attr + 0x44] = 0x11;
    img[attr + 0x45] = 0x01;
    img[attr + 0x46] = 0x04;
    writeLe32(img, attr + 0x48, 0xFFFFFFFF);

    const size_t jOff = 4 * 8 * 512;
    const char* name = "secret.txt";
    const size_t nameLen = std::strlen(name);
    const uint16_t nameBytes = static_cast<uint16_t>(nameLen * 2);
    const uint16_t nameOff = 60;
    uint32_t recordLen = nameOff + nameBytes;
    while (recordLen % 8 != 0) ++recordLen;
    writeLe32(img, jOff + 0, recordLen);
    writeLe32(img, jOff + 4, 2);
    writeLe64(img, jOff + 8, 1234);
    writeLe64(img, jOff + 16, 5);
    writeLe64(img, jOff + 24, 99);
    writeLe64(img, jOff + 32, 132223104000000000ULL);
    writeLe32(img, jOff + 40, 0x00000001);
    writeLe16(img, jOff + 56, nameBytes);
    writeLe16(img, jOff + 58, nameOff);
    for (size_t i = 0; i < nameLen; ++i)
        writeLe16(img, jOff + nameOff + i * 2, static_cast<uint16_t>(name[i]));
    return img;
}

TEST(NtfsParser, UsnJournalEmitsTimelineEvent) {
    auto img = buildNtfsUsnJournalDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool saw = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == "usn_journal" && fr.name == "secret.txt") saw = true;
    }, &running, 0, 0, false));
    EXPECT_TRUE(saw);
}

TEST(NtfsParser, PaddedUsnJournalIsNotEmptyJournal) {
    auto img = buildNtfsUsnJournalDisk();
    img.resize(4 * 8 * 512);
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    bool sawEvent = false;
    bool sawUnread = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == "usn_journal") sawEvent = true;
        if (fr.source == kUsnUnreadSource) sawUnread = true;
    }, &running, 0, 0, false));
    EXPECT_FALSE(sawEvent);
    EXPECT_TRUE(sawUnread);
}

TEST(NtfsParser, FaultedUsnJournalEmitsUnread) {
    auto img = buildNtfsUsnJournalDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    reader.setMemoryFaultRange(32, 8);
    bool sawEvent = false;
    bool sawUnread = false;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.source == "usn_journal" && fr.name == "secret.txt") sawEvent = true;
        if (fr.source == kUsnUnreadSource) sawUnread = true;
    }, &running, 0, 0, false));
    EXPECT_FALSE(sawEvent);
    EXPECT_TRUE(sawUnread);
}

TEST(NtfsParser, OversizedAttributeLengthDoesNotLoopForever) {
    // Regression: an attribute whose declared length wraps uint32 attrOffset
    // back into the record used to cycle the attribute loop forever.
    auto img = buildNtfsMftCarveDisk();
    // First attribute at 0x38: type $FILE_NAME marker bytes replaced by a
    // crafted pair that returns to the same offset:
    //   attr@0x38 length 16 -> next 0x48, attr@0x48 length 0xFFFFFFF0 -> 0x38.
    writeLe32(img, 8 * 512 + 0x38 + 0, 0x10);
    writeLe32(img, 8 * 512 + 0x38 + 4, 16);
    writeLe32(img, 8 * 512 + 0x48 + 0, 0x90);
    writeLe32(img, 8 * 512 + 0x48 + 4, 0xFFFFFFF0);

    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    EXPECT_TRUE(ntfs.scan(reader, [](const FileRecord&) {}, &running));
}

// CA-032: with carveOrphanMft=true the orphan pass rescans the live MFT zone.
// The sentinel bug made every live record emit TWICE (doc.txt x2). Count
// assertions, not booleans — booleans are how this escaped.
TEST(NtfsParser, OrphanPassDoesNotDuplicateLiveMftRecords) {
    auto img = buildNtfsBootMftWalkDisk();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    std::vector<std::string> names;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0 && !fr.name.empty()) names.push_back(fr.name);
    }, &running, 0, 0, true));

    int doc = 0, orphan = 0;
    for (const auto& n : names) {
        if (n == "doc.txt") ++doc;
        if (n == "orphan.bin") ++orphan;
    }
    EXPECT_EQ(doc, 1);    // main pass only; orphan pass must skip via dedupByMft
    EXPECT_EQ(orphan, 1); // genuinely outside the MFT runs: discovered once
}

// ---------------------------------------------------------------------------
// FAZ 1.1 (A2/D2, in-suite point): the streaming redesign must remove the
// pass-1 tempFiles accumulation. Fixture: 200K records x 1 KB, 688 B resident
// $DATA each, read from a temp FILE (attachRawFile) so the image itself stays
// out of the measured working set. gtest_discover_tests runs this TEST in a
// fresh process, so the PeakWorkingSetSize delta isolates the scan.
//
// RED evidence (pre-refactor): standalone probe over the same fixture measured
// PEAK_DELTA=254.7 MB at 200K (tempFiles: FileRecord + residentData). The
// bound below fails on the old two-phase full-buffer code and must hold on
// the streaming design. Sibling TEST StreamingScanPeakMemoryBelowBoundAt2M
// is the 2M / <700 MB gate (separate process: PeakWorkingSetSize is monotonic).
// ---------------------------------------------------------------------------
TEST(NtfsParser, StreamingScanPeakMemoryBelowBoundAt200K) {
    const uint64_t kRecords = 200000;
    std::string path, err;
    ASSERT_TRUE(NtfsVolumeFile::write(kRecords, path, err)) << err;
    NtfsVolumeFile cleanup(path);

    DiskReader reader;
    std::string attachErr;
    ASSERT_TRUE(reader.attachRawFile(path, &attachErr)) << attachErr;

    const double peakBeforeMB = PeakWorkingSetMB();
    std::atomic<bool> running{true};
    std::atomic<uint64_t> emitted{0};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0 && !fr.name.empty()) ++emitted;
    }, &running, 0, 0, false));
    const double peakAfterMB = PeakWorkingSetMB();

    EXPECT_EQ(emitted.load(), kRecords);  // $MFT record (fallback name) + 199,999 named
    EXPECT_LT(peakAfterMB - peakBeforeMB, 150.0);
}

// ---------------------------------------------------------------------------
// FAZ 1.1 (A2/D2, in-suite 2M point): 10× the 200K fixture, same attachRawFile
// isolation. Linear tempFiles growth from the old 254.7 MB / 200K probe would
// be ~2.5 GB here; the streaming gate is peak delta < 700 MB. 200K bound
// 150 MB × 10 = 1500 MB, so this 700 MB ceiling also rejects that linear
// envelope. Own gtest_discover process: do not pair with the 200K TEST.
// ---------------------------------------------------------------------------
TEST(NtfsParser, StreamingScanPeakMemoryBelowBoundAt2M) {
    const uint64_t kRecords = 2000000;
    std::string path, err;
    ASSERT_TRUE(NtfsVolumeFile::write(kRecords, path, err)) << err;
    NtfsVolumeFile cleanup(path);

    DiskReader reader;
    std::string attachErr;
    ASSERT_TRUE(reader.attachRawFile(path, &attachErr)) << attachErr;

    const double peakBeforeMB = PeakWorkingSetMB();
    std::atomic<bool> running{true};
    std::atomic<uint64_t> emitted{0};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0 && !fr.name.empty()) ++emitted;
    }, &running, 0, 0, false));
    const double peakAfterMB = PeakWorkingSetMB();
    const double delta = peakAfterMB - peakBeforeMB;

    EXPECT_EQ(emitted.load(), kRecords);
    EXPECT_LT(delta, 700.0) << "peak delta=" << delta << " MB (gate <700)";
}

// ---------------------------------------------------------------------------
// FAZ 1.1 (D1, K1 no-loss): on a synthetic volume the total number of emitted
// file records must equal the number of MftIndex entries. Fixture: record 0
// is $MFT itself, records 1..7 are named deleted files — 8 "FILE" records on
// disk, 8 MftIndex entries, so 8 emits (emission ORDER may differ from the
// old code; multiset equality is the contract).
// ---------------------------------------------------------------------------
TEST(NtfsParser, StreamingEmitCountEqualsMftIndexEntries) {
    const uint64_t kRecords = 8;
    std::string path, err;
    ASSERT_TRUE(NtfsVolumeFile::write(kRecords, path, err)) << err;
    NtfsVolumeFile cleanup(path);

    DiskReader reader;
    std::string attachErr;
    ASSERT_TRUE(reader.attachRawFile(path, &attachErr)) << attachErr;

    std::atomic<bool> running{true};
    std::vector<FileRecord> out;
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if ((fr.id >= 0 && !fr.name.empty()) || fr.source == "orphan") out.push_back(fr);
    }, &running, 0, 0, false));

    ASSERT_EQ(out.size(), kRecords);
    int named = 0;
    for (const auto& fr : out) {
        EXPECT_FALSE(fr.source == "orphan") << "intact volume: sweep must stay idle";
        if (!fr.name.empty()) ++named;
    }
    EXPECT_EQ(named, static_cast<int>(kRecords)); // $MFT fallback name + f0000002..f0000008
}

// ---------------------------------------------------------------------------
// FAZ 1.1 (D1, multiset equivalence): the streamed record must carry the same
// fields the pre-refactor buffered loop emitted (name/size/resident payload/
// status/confidence band/startSector).
// ---------------------------------------------------------------------------
TEST(NtfsParser, StreamingRecordFieldsMatchLegacySemantics) {
    auto img = byteback::testfix::buildNtfsDeletedResidentVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    FileRecord doc;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "doc.txt") doc = fr;
    }, &running, 0, 0, false));

    EXPECT_EQ(doc.source, "ntfs_mft");
    EXPECT_EQ(doc.status, 0);
    EXPECT_EQ(doc.sizeBytes, 5u);
    EXPECT_EQ(std::string(doc.residentData.begin(), doc.residentData.end()), "hello");
    EXPECT_EQ(doc.startSector, 10u);          // (8*512 + 1024) / 512
    // deleted(45) + resident payload(25) - USA fixup missing(20) = 50 —
    // identical to the pre-refactor score for this fixture.
    EXPECT_EQ(doc.confidence, 50);
    EXPECT_EQ(doc.mftRef, 1); // rec1 = doc.txt; parentId stays parent MFT (5)
}

TEST(NtfsParser, EmittedMftRefOpensRecordView) {
    auto img = byteback::testfix::buildNtfsDeletedResidentVolume();
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));
    FileRecord doc;
    std::atomic<bool> running{true};
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.name == "doc.txt") doc = fr;
    }, &running, 0, 0, false));
    ASSERT_EQ(doc.mftRef, 1);
    bool unread = false;
    auto view = getMftRecordView(reader, static_cast<uint64_t>(doc.mftRef), 0, &unread);
    ASSERT_TRUE(view.has_value());
    EXPECT_FALSE(unread);
    EXPECT_EQ(view->signature, "FILE");
    EXPECT_EQ(view->mftRef, 1u);
}

// ---------------------------------------------------------------------------
// FAZ 1.1 (K1 orphan sweep proof): Pass-2 re-reads each record from disk. When
// those re-reads FAIL mid-emit (injected bad-sector range — Pass-1 already
// succeeded), the affected entries stay unvisited and the K1 sweep must emit
// them as nameless orphans. No record may be lost to an IO failure: emits +
// sweep == MftIndex entries.
// ---------------------------------------------------------------------------
TEST(NtfsParser, OrphanSweepEmitsEntriesLostToMidScanReadFailure) {
    const uint64_t kRecords = 8;
    std::string path, err;
    ASSERT_TRUE(NtfsVolumeFile::write(kRecords, path, err)) << err;
    NtfsVolumeFile cleanup(path);

    // setMemoryFaultRange is a memory-backend hook: load the same 16 KiB image
    // into RAM instead of attaching the file.
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good());
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    ASSERT_EQ(img.size(), static_cast<size_t>(16384));
    DiskReader reader;
    reader.attachMemoryVolume(std::move(img));

    std::atomic<bool> running{true};
    std::atomic<bool> faultInjected{false};
    std::vector<FileRecord> named, swept;
    NTFSParser ntfs;
    ASSERT_TRUE(ntfs.scanAt(reader, [&](const FileRecord& fr) {
        if (fr.id >= 0 && !fr.name.empty()) {
            // Pass-2 is streaming: once the first real record arrives, poison
            // the sectors of records 5..7 (bytes 9216..12288 = sectors 18..23)
            // so their re-reads fail and the sweep path has to cover them.
            if (!faultInjected.exchange(true)) {
                reader.setMemoryFaultRange(18, 6);
            }
            named.push_back(fr);
        }
        if (fr.source == "orphan") swept.push_back(fr);
    }, &running, 0, 0, false));

    // Records 0..4 emitted by the Pass-2 walk; 5..7 recovered by the sweep.
    EXPECT_EQ(named.size(), 5u);
    ASSERT_EQ(swept.size(), 3u);              // emits + sweep == 8 == entries (K1)
    for (const auto& fr : swept) {
        EXPECT_TRUE(fr.name.empty());
        EXPECT_EQ(fr.source, "orphan");
        EXPECT_EQ(fr.status, 0);
        EXPECT_LE(fr.confidence, 45);         // low orphan confidence band
        EXPECT_GE(fr.id, 600000);             // distinct id band
    }
    EXPECT_EQ(swept[0].startSector, 18u);     // byte 9216 = record 5
    EXPECT_EQ(swept[1].startSector, 20u);
    EXPECT_EQ(swept[2].startSector, 22u);
}

// ---------------------------------------------------------------------------
// FAZ 1.1 (K2 emit timing observation): file records must arrive WHILE the
// scan is still running, not in one terminal flush. Polled from another
// thread over a 100K-record volume: the observation below could only catch
// the old design at the very tail of the scan (post-pass flush), while the
// streaming Pass-2 keeps emitting throughout the walk.
// ---------------------------------------------------------------------------
TEST(NtfsParser, EmitsRecordsWhileScanIsStillRunning) {
    const uint64_t kRecords = 100000;
    std::string path, err;
    ASSERT_TRUE(NtfsVolumeFile::write(kRecords, path, err)) << err;
    NtfsVolumeFile cleanup(path);

    DiskReader reader;
    std::string attachErr;
    ASSERT_TRUE(reader.attachRawFile(path, &attachErr)) << attachErr;

    std::atomic<bool> running{true};
    std::atomic<uint64_t> emitted{0};
    std::atomic<bool> finished{false};
    NTFSParser ntfs;
    bool scanOk = false;
    std::thread worker([&] {
        scanOk = ntfs.scanAt(reader, [&](const FileRecord& fr) {
            if (fr.id >= 0 && !fr.name.empty()) ++emitted;
        }, &running, 0, 0, false);
        finished = true;
    });

    bool observedMidScanEmits = false;
    while (!finished.load()) {
        if (emitted.load() > 0) {
            observedMidScanEmits = true;  // K2: callback flowed in mid-scan
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    worker.join();

    EXPECT_TRUE(scanOk);
    EXPECT_EQ(emitted.load(), kRecords);
    EXPECT_TRUE(observedMidScanEmits);
}
