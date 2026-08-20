#include "fs/bitlocker_unlock.h"
#include "fs/bitlocker_fve.h"
#include "crypto/wolf_sha256.h"
#include "crypto/wolf_aes_ccm.h"
#include <algorithm>
#include <cstring>

namespace wolf {

namespace {

constexpr uint16_t kEntryClearKey = 0x0800;
constexpr uint16_t kEntryVmk = 0x000f;
constexpr uint16_t kEntryFvek = 0x0003;
constexpr uint32_t kKeyTypeVmk = 0x00000001;
constexpr uint32_t kKeyTypeFvek = 0x00000002;
constexpr size_t kStretchIterations = 1'000'000;

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

void stretchRecoveryPassword(const std::string& passwordUtf8, uint8_t out[32]) {
    std::vector<uint8_t> wide;
    wide.reserve(passwordUtf8.size() * 2 + 2);
    for (unsigned char c : passwordUtf8) {
        wide.push_back(c);
        wide.push_back(0);
    }
    wide.push_back(0);
    wide.push_back(0);
    crypto::sha256(wide.data(), wide.size(), out);
    for (size_t i = 1; i < kStretchIterations; ++i) {
        crypto::sha256(out, 32, out);
    }
}

bool decryptKeyBlob(const uint8_t key[32], const uint8_t* blob, size_t blobLen,
                    std::vector<uint8_t>& plain) {
    if (!blob || blobLen < 12 + 16 + 8) return false;
    uint16_t encSize = le16(blob + 4);
    if (encSize < 28 || static_cast<size_t>(8 + encSize) > blobLen) return false;
    const uint8_t* enc = blob + 8;
    size_t ctLen = encSize;
    if (ctLen < 28) return false;
    plain.resize(ctLen - 16);
    if (!crypto::aesCcmDecrypt(key, enc, enc + 12, ctLen - 12, plain.data(), plain.size())) {
        plain.clear();
        return false;
    }
    return true;
}

bool parseDecryptedKey(const std::vector<uint8_t>& plain, uint32_t wantType,
                       std::vector<uint8_t>& keyOut) {
    if (plain.size() < 16) return false;
    uint32_t keySize = le32(plain.data());
    uint32_t keyType = le32(plain.data() + 4);
    if (keyType != wantType) return false;
    if (keySize == 0 || keySize > 128 || 16 + keySize > plain.size()) return false;
    keyOut.assign(plain.begin() + 16, plain.begin() + 16 + keySize);
    return true;
}

bool readMetadata(DiskReader& reader, uint64_t volumeOffsetBytes, std::vector<uint8_t>& meta) {
    uint32_t ss = reader.getSectorSize();
    if (ss == 0) ss = 512;
    std::vector<uint8_t> boot(ss);
    if (!reader.readSectors(volumeOffsetBytes, ss, boot.data()).success) return false;
    if (boot.size() < 0xA8 || std::memcmp(boot.data() + 3, "-FVE-FS-", 8) != 0) return false;
    uint64_t metaOff = 0;
    for (int i = 0; i < 8; ++i) metaOff |= static_cast<uint64_t>(boot[0xA0 + i]) << (8 * i);
    if (metaOff == 0) return false;
    meta.resize(64 * 1024);
    if (!reader.readSectors(volumeOffsetBytes + metaOff, static_cast<uint32_t>(meta.size()), meta.data()).success) {
        return false;
    }
    if (std::memcmp(meta.data(), "-FVE-FS-", 8) != 0) return false;
    return true;
}

bool findEntry(const std::vector<uint8_t>& meta, uint16_t entryType,
               const uint8_t*& value, size_t& valueLen) {
    if (meta.size() < 64 + 72) return false;
    size_t pos = 64 + 72;
    while (pos + 8 <= meta.size()) {
        uint16_t type = le16(meta.data() + pos);
        uint32_t vsize = le32(meta.data() + pos + 4);
        size_t entrySize = 8 + vsize;
        if (vsize > meta.size() || pos + entrySize > meta.size()) break;
        if (type == entryType) {
            value = meta.data() + pos + 8;
            valueLen = vsize;
            return true;
        }
        pos += entrySize;
    }
    return false;
}

} // namespace

BitLockerUnlockResult unlockBitLockerWithRecoveryPassword(DiskReader& reader,
                                                          const std::string& passwordUtf8,
                                                          uint64_t volumeOffsetBytes) {
    BitLockerUnlockResult result;
    if (passwordUtf8.empty()) {
        result.error = "empty password";
        return result;
    }
    std::vector<uint8_t> meta;
    if (!readMetadata(reader, volumeOffsetBytes, meta)) {
        result.error = "FVE metadata not found";
        return result;
    }

    const uint8_t* clearValue = nullptr;
    size_t clearLen = 0;
    if (!findEntry(meta, kEntryClearKey, clearValue, clearLen)) {
        result.error = "recovery password protector (0x0800) not found — TPM/startup-key/password unsupported";
        return result;
    }

    uint8_t stretched[32];
    stretchRecoveryPassword(passwordUtf8, stretched);

    std::vector<uint8_t> vmkPlain;
    if (!decryptKeyBlob(stretched, clearValue, clearLen, vmkPlain)) {
        result.error = "recovery password rejected or corrupt clear-key blob";
        return result;
    }

    std::vector<uint8_t> vmk;
    if (!parseDecryptedKey(vmkPlain, kKeyTypeVmk, vmk) || vmk.size() != 32) {
        result.error = "VMK unwrap failed";
        return result;
    }

    const uint8_t* fvekValue = nullptr;
    size_t fvekLen = 0;
    if (!findEntry(meta, kEntryFvek, fvekValue, fvekLen)) {
        result.error = "FVEK entry (0x0003) not found";
        return result;
    }

    std::vector<uint8_t> fvekPlain;
    if (!decryptKeyBlob(vmk.data(), fvekValue, fvekLen, fvekPlain)) {
        result.error = "FVEK decrypt failed";
        return result;
    }

    std::vector<uint8_t> fvek;
    if (!parseDecryptedKey(fvekPlain, kKeyTypeFvek, fvek) || (fvek.size() != 32 && fvek.size() != 64)) {
        result.error = "FVEK parse failed";
        return result;
    }

    result.fvek = std::move(fvek);
    result.success = true;
    return result;
}

} // namespace wolf
