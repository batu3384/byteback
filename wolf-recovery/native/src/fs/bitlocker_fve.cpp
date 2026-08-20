#include "fs/bitlocker_fve.h"
#include <cstdio>
#include <cstring>

namespace wolf {

std::string bitLockerMethodName(uint32_t method) {
    switch (method) {
        case 0: return "unknown";
        case 1: return "AES-128-CBC-Elephant";
        case 2: return "AES-256-CBC-Elephant";
        case 3: return "AES-128-CBC";
        case 4: return "AES-256-CBC";
        case 5: return "AES-128-XTS";
        case 6: return "AES-256-XTS";
        default: return "method_" + std::to_string(method);
    }
}

bool parseBitLockerFve(const uint8_t* boot, size_t bootLen,
                       const uint8_t* metadata, size_t metaLen,
                       BitLockerFveInfo& out) {
    out = BitLockerFveInfo{};
    if (!boot || bootLen < 0xA8) return false;
    if (std::memcmp(boot + 3, "-FVE-FS-", 8) != 0) return false;
    out.detected = true;
    uint64_t metaOff = 0;
    for (int i = 0; i < 8; ++i) metaOff |= static_cast<uint64_t>(boot[0xA0 + i]) << (8 * i);
    out.metadataOffset = metaOff;

    if (!metadata || metaLen < 64 + 40) return true;
    const uint8_t* m = metadata;
    if (std::memcmp(m, "-FVE-FS-", 8) != 0) return true;

    // libbde: FVE metadata header follows 64-byte block header.
    // encryption_method is uint32 at header+0x24 (volume GUID is 16 bytes at +0x10).
    const uint8_t* hdr = m + 64;
    if (metaLen < 64 + 0x28) return true;
    char guid[33];
    for (int i = 0; i < 16; ++i) {
        static const char* HEX = "0123456789abcdef";
        unsigned b = hdr[0x10 + i];
        guid[i * 2] = HEX[b >> 4];
        guid[i * 2 + 1] = HEX[b & 0xF];
    }
    guid[32] = 0;
    out.volumeGuidHex = guid;
    out.encryptionMethod = static_cast<uint32_t>(hdr[0x24]) |
                           (static_cast<uint32_t>(hdr[0x25]) << 8) |
                           (static_cast<uint32_t>(hdr[0x26]) << 16) |
                           (static_cast<uint32_t>(hdr[0x27]) << 24);
    out.encryptionName = bitLockerMethodName(out.encryptionMethod);
    return true;
}

void emitBitLockerRecord(const BitLockerFveInfo& info, uint64_t startSector,
                         FileSystemParser::FileRecordCallback onFileFound) {
    FileRecord fr;
    fr.id = -1;
    fr.path = "/";
    fr.sizeBytes = 0;
    fr.startSector = startSector;
    fr.status = 2;
    fr.confidence = 100;
    fr.category = "Encrypted";
    if (info.encryptionMethod != 0) {
        fr.source = "bitlocker_fve";
        fr.name = "[BitLocker] " + info.encryptionName;
        if (!info.volumeGuidHex.empty()) fr.path = "/bitlocker/" + info.volumeGuidHex;
    } else {
        fr.source = "bitlocker_detect";
        fr.name = "[BitLocker] Birim sifreli - FVE metadata / kurtarma anahtari gerekli";
    }
    onFileFound(fr);
}

} // namespace wolf
