#include "fs/bitlocker_unlock.h"
#include "fs/bitlocker_fve.h"
#include "crypto/byteback_sha256.h"
#include "crypto/byteback_aes_ccm.h"
#include <functional>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace byteback {

namespace {

constexpr uint16_t kProtectRecovery = 0x0800;
constexpr uint16_t kProtectPassword = 0x2000;
constexpr uint16_t kProtectTpm = 0x0100;
constexpr uint16_t kProtectStartup = 0x0200;
constexpr uint16_t kProtectTpmPin = 0x0500;
constexpr uint16_t kEntryFvek = 0x0003;
constexpr uint16_t kValueVmk = 0x0008;
constexpr uint16_t kValueStretch = 0x0003;
constexpr uint16_t kValueAesCcm = 0x0005;
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

void utf8ToUtf16Le(const std::string& passwordUtf8, std::vector<uint8_t>& wide) {
    wide.clear();
    if (passwordUtf8.empty()) return;
#ifdef _WIN32
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, passwordUtf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return;
    std::vector<wchar_t> buf(static_cast<size_t>(wlen));
    if (MultiByteToWideChar(CP_UTF8, 0, passwordUtf8.c_str(), -1, buf.data(), wlen) <= 0) return;
    wide.reserve((buf.size() > 0 ? buf.size() - 1 : 0) * 2);
    for (size_t i = 0; i + 1 < buf.size(); ++i) {
        const wchar_t wc = buf[i];
        wide.push_back(static_cast<uint8_t>(wc & 0xFF));
        wide.push_back(static_cast<uint8_t>((wc >> 8) & 0xFF));
    }
#else
    // ponytail: non-Windows builds use ASCII-only stretch; upgrade path = ICU/iconv.
    wide.reserve(passwordUtf8.size() * 2);
    for (unsigned char c : passwordUtf8) {
        wide.push_back(c);
        wide.push_back(0);
    }
#endif
}

void stretchRecoveryPassword(const std::string& passwordUtf8, uint8_t out[32]) {
    std::vector<uint8_t> wide;
    utf8ToUtf16Le(passwordUtf8, wide);
    crypto::sha256(wide.data(), wide.size(), out);
    for (size_t i = 1; i < kStretchIterations; ++i) {
        crypto::sha256(out, 32, out);
    }
}

void hashPasswordUtf8(const std::string& passwordUtf8, uint8_t out[32]) {
    std::vector<uint8_t> wide;
    utf8ToUtf16Le(passwordUtf8, wide);
    crypto::sha256(wide.data(), wide.size(), out);
    crypto::sha256(out, 32, out);
}

struct PasswordKeyData {
    uint8_t lastSha256[32];
    uint8_t initialSha256[32];
    uint8_t salt[16];
    uint64_t count;
};

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

bool decryptAesCcmValue(const uint8_t key[32], const uint8_t* val, size_t vlen,
                         std::vector<uint8_t>& plain) {
    if (!val || vlen < 12 + 16 + 8) return false;
    plain.resize(vlen - 12 - 16);
    if (!crypto::aesCcmDecrypt(key, val, val + 12, vlen - 12, plain.data(), plain.size())) {
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

bool findLegacyEntry(const std::vector<uint8_t>& meta, uint16_t entryType,
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

bool walkNestedEntries(const uint8_t* data, size_t len,
                       const std::function<bool(uint16_t valueType, const uint8_t* vdata, size_t vlen)>& fn) {
    size_t pos = 0;
    while (pos + 8 <= len) {
        uint16_t esize = le16(data + pos);
        if (esize < 8 || pos + esize > len) break;
        uint16_t vtype = le16(data + pos + 4);
        const uint8_t* vdata = data + pos + 8;
        size_t vlen = esize - 8;
        if (!fn(vtype, vdata, vlen)) return false;
        pos += esize;
    }
    return true;
}

struct ProtectorMaterial {
    const uint8_t* aesBlob = nullptr;
    size_t aesBlobLen = 0;
    const uint8_t* stretchSalt = nullptr;
    bool passwordKdf = false;
};

bool parseVmkProperties(const uint8_t* props, size_t propsLen, uint16_t protectType,
                        ProtectorMaterial& out) {
    out = ProtectorMaterial{};
    out.passwordKdf = (protectType == kProtectPassword);
    const uint8_t* stretchSalt = nullptr;
    const uint8_t* aesVal = nullptr;
    size_t aesLen = 0;

    walkNestedEntries(props, propsLen, [&](uint16_t vtype, const uint8_t* vdata, size_t vlen) -> bool {
        if (vtype == kValueStretch && vlen >= 20) {
            stretchSalt = vdata + 4;
            walkNestedEntries(vdata + 20, vlen - 20, [&](uint16_t nestedType, const uint8_t* nestedData,
                                                          size_t nestedLen) -> bool {
                if (nestedType == kValueAesCcm) {
                    aesVal = nestedData;
                    aesLen = nestedLen;
                }
                return true;
            });
        } else if (vtype == kValueAesCcm) {
            aesVal = vdata;
            aesLen = vlen;
        }
        return true;
    });

    if (!aesVal) return false;
    if (out.passwordKdf && !stretchSalt) return false;
    out.stretchSalt = stretchSalt;
    out.aesBlob = aesVal;
    out.aesBlobLen = aesLen;
    return true;
}

bool collectProtector(const std::vector<uint8_t>& meta, uint16_t protectType, ProtectorMaterial& out) {
    if (meta.size() < 112) return false;
    uint32_t headerSize = le32(meta.data() + 64 + 8);
    size_t pos = 64 + headerSize;
    while (pos + 8 <= meta.size()) {
        uint16_t esize = le16(meta.data() + pos);
        if (esize < 8 || pos + esize > meta.size()) break;
        uint16_t vtype = le16(meta.data() + pos + 4);
        const uint8_t* vdata = meta.data() + pos + 8;
        size_t vlen = esize - 8;
        if (vtype == kValueVmk && vlen >= 28) {
            uint16_t prot = le16(vdata + 26);
            if (prot == protectType && parseVmkProperties(vdata + 28, vlen - 28, protectType, out)) {
                return true;
            }
        }
        pos += esize;
    }

    const uint8_t* legacy = nullptr;
    size_t legacyLen = 0;
    if (findLegacyEntry(meta, protectType, legacy, legacyLen)) {
        out.aesBlob = legacy;
        out.aesBlobLen = legacyLen;
        out.passwordKdf = (protectType == kProtectPassword);
        return true;
    }
    return false;
}

std::string unsupportedProtectorHint(const std::vector<uint8_t>& meta) {
    if (meta.size() < 112) return "TPM/startup-key/password unsupported";
    uint32_t headerSize = le32(meta.data() + 64 + 8);
    size_t pos = 64 + headerSize;
    bool tpm = false;
    bool startup = false;
    while (pos + 8 <= meta.size()) {
        uint16_t esize = le16(meta.data() + pos);
        if (esize < 8 || pos + esize > meta.size()) break;
        uint16_t vtype = le16(meta.data() + pos + 4);
        const uint8_t* vdata = meta.data() + pos + 8;
        size_t vlen = esize - 8;
        if (vtype == kValueVmk && vlen >= 28) {
            uint16_t prot = le16(vdata + 26);
            if (prot == kProtectTpm || prot == kProtectTpmPin) tpm = true;
            if (prot == kProtectStartup) startup = true;
        }
        pos += esize;
    }
    if (tpm && startup) return "TPM + startup-key protectors only — parola/recovery yok";
    if (tpm) return "TPM protector only — parola/recovery yok";
    if (startup) return "startup-key (.BEK) protector only — parola/recovery yok";
    return "TPM/startup-key/password unsupported";
}

bool deriveKeyFromPassword(const std::string& passwordUtf8, const ProtectorMaterial& material,
                           uint8_t out[32]) {
    if (material.passwordKdf) {
        if (!material.stretchSalt) return false;
        deriveBitLockerPasswordKey(passwordUtf8, material.stretchSalt, out);
        return true;
    }
    stretchRecoveryPassword(passwordUtf8, out);
    return true;
}

bool decryptProtectorBlob(const uint8_t key[32], const ProtectorMaterial& material,
                          std::vector<uint8_t>& plain) {
    if (material.passwordKdf) {
        return decryptAesCcmValue(key, material.aesBlob, material.aesBlobLen, plain);
    }
    return decryptKeyBlob(key, material.aesBlob, material.aesBlobLen, plain);
}

BitLockerUnlockResult unlockWithProtector(DiskReader& reader, const std::string& passwordUtf8,
                                          uint16_t protectType, uint64_t volumeOffsetBytes) {
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

    ProtectorMaterial material;
    if (!collectProtector(meta, protectType, material)) {
        result.error = unsupportedProtectorHint(meta);
        return result;
    }

    uint8_t derived[32];
    if (!deriveKeyFromPassword(passwordUtf8, material, derived)) {
        result.error = "password key derivation failed";
        return result;
    }

    std::vector<uint8_t> vmkPlain;
    if (!decryptProtectorBlob(derived, material, vmkPlain)) {
        result.error = protectType == kProtectPassword ? "password rejected or corrupt VMK blob"
                                                       : "recovery password rejected or corrupt clear-key blob";
        return result;
    }

    std::vector<uint8_t> vmk;
    if (!parseDecryptedKey(vmkPlain, kKeyTypeVmk, vmk) || vmk.size() != 32) {
        result.error = "VMK unwrap failed";
        return result;
    }

    const uint8_t* fvekValue = nullptr;
    size_t fvekLen = 0;
    if (!findLegacyEntry(meta, kEntryFvek, fvekValue, fvekLen)) {
        uint32_t headerSize = le32(meta.data() + 64 + 8);
        size_t pos = 64 + headerSize;
        while (pos + 8 <= meta.size()) {
            uint16_t esize = le16(meta.data() + pos);
            if (esize < 8 || pos + esize > meta.size()) break;
            uint16_t etype = le16(meta.data() + pos + 2);
            if (etype == kEntryFvek) {
                fvekValue = meta.data() + pos + 8;
                fvekLen = esize - 8;
                break;
            }
            pos += esize;
        }
    }
    if (!fvekValue || fvekLen == 0) {
        result.error = "FVEK entry (0x0003) not found";
        return result;
    }

    std::vector<uint8_t> fvekPlain;
    if (!decryptKeyBlob(vmk.data(), fvekValue, fvekLen, fvekPlain) &&
        !decryptAesCcmValue(vmk.data(), fvekValue, fvekLen, fvekPlain)) {
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

} // namespace

void deriveBitLockerPasswordKey(const std::string& passwordUtf8, const uint8_t salt[16],
                                uint8_t out[32]) {
    uint8_t passwordHash[32];
    hashPasswordUtf8(passwordUtf8, passwordHash);

    PasswordKeyData data{};
    std::memcpy(data.initialSha256, passwordHash, 32);
    std::memcpy(data.salt, salt, 16);
    for (uint64_t i = 0; i < 0xfffff; ++i) {
        data.count = i;
        crypto::sha256(reinterpret_cast<const uint8_t*>(&data), sizeof(data), data.lastSha256);
    }
    data.count = 0xfffff;
    crypto::sha256(reinterpret_cast<const uint8_t*>(&data), sizeof(data), out);
}

BitLockerUnlockResult unlockBitLockerWithPassword(DiskReader& reader, const std::string& passwordUtf8,
                                                  uint64_t volumeOffsetBytes) {
    return unlockWithProtector(reader, passwordUtf8, kProtectPassword, volumeOffsetBytes);
}

BitLockerUnlockResult unlockBitLockerWithRecoveryPassword(DiskReader& reader,
                                                          const std::string& passwordUtf8,
                                                          uint64_t volumeOffsetBytes) {
    return unlockWithProtector(reader, passwordUtf8, kProtectRecovery, volumeOffsetBytes);
}

} // namespace byteback
