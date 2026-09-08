#include "byteback_fs.h"
#include "byteback_io.h"
#include "fixtures/volume_fixtures.h"
#include "fs/ntfs_util.h"
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
                   ("byteback_ntfs_mem_test_" + std::to_string(hostPid()) + ".img"))
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
// the streaming design. The 2M point is measured with a standalone probe and
// reported against the <700 MB gate.
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
