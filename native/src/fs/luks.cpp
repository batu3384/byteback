#include "fs/luks.h"
#include "fs/partition_scanner.h"
#include "crypto/byteback_aes.h"
#include "crypto/byteback_sha256.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace byteback {
namespace {

constexpr char kMagic[6] = {'L', 'U', 'K', 'S', '\xba', '\xbe'};
constexpr uint32_t kKeyEnabled = 0x00AC71F3;
constexpr uint32_t kKeyDisabled = 0x0000DEAD;
constexpr uint32_t kPhdr = 592;
constexpr uint32_t kSlots = 8;
constexpr uint32_t kSalt = 32;
constexpr uint32_t kDigest = 20;

uint16_t rdBe16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
uint32_t rdBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
void wrBe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}
void wrBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

void xorBlock(uint8_t* dst, const uint8_t* src, size_t n) {
    for (size_t i = 0; i < n; ++i) dst[i] = static_cast<uint8_t>(dst[i] ^ src[i]);
}

void hashChunk(const uint8_t* src, uint8_t* dst, uint32_t idx, size_t len) {
    std::vector<uint8_t> msg(4 + len);
    msg[0] = static_cast<uint8_t>(idx >> 24);
    msg[1] = static_cast<uint8_t>(idx >> 16);
    msg[2] = static_cast<uint8_t>(idx >> 8);
    msg[3] = static_cast<uint8_t>(idx);
    if (len) std::memcpy(msg.data() + 4, src, len);
    uint8_t dig[32];
    crypto::sha256(msg.data(), msg.size(), dig);
    std::memcpy(dst, dig, len);
}

void diffuse(uint8_t* p, size_t size) {
    std::vector<uint8_t> dst(size);
    constexpr unsigned kDig = 32;
    const unsigned blocks = static_cast<unsigned>(size / kDig);
    const unsigned pad = static_cast<unsigned>(size % kDig);
    for (unsigned i = 0; i < blocks; ++i) hashChunk(p + i * kDig, dst.data() + i * kDig, i, kDig);
    if (pad) hashChunk(p + blocks * kDig, dst.data() + blocks * kDig, blocks, pad);
    std::memcpy(p, dst.data(), size);
}

void afMerge(const uint8_t* src, uint8_t* dst, size_t block, unsigned stripes) {
    std::vector<uint8_t> buf(block, 0);
    for (unsigned i = 0; i + 1 < stripes; ++i) {
        xorBlock(buf.data(), src + i * block, block);
        diffuse(buf.data(), block);
    }
    std::memcpy(dst, src + (stripes - 1) * block, block);
    xorBlock(dst, buf.data(), block);
}

void afSplitZeros(const uint8_t* src, uint8_t* dst, size_t block, unsigned stripes) {
    std::vector<uint8_t> buf(block, 0);
    std::memset(dst, 0, block * (stripes - 1));
    for (unsigned i = 0; i + 1 < stripes; ++i) {
        xorBlock(buf.data(), dst + i * block, block);
        diffuse(buf.data(), block);
    }
    std::memcpy(dst + (stripes - 1) * block, src, block);
    xorBlock(dst + (stripes - 1) * block, buf.data(), block);
}

void xts128Sector(const uint8_t key32[32], uint64_t sector, uint8_t* sec, bool enc) {
    uint8_t tweak[16] = {};
    for (int i = 0; i < 8; ++i) tweak[i] = static_cast<uint8_t>(sector >> (8 * i));
    crypto::xtsAes128Crypt(key32, tweak, sec, sec, 512, enc);
}

void xts256Sector(const uint8_t key64[64], uint64_t sector, uint8_t* sec, bool enc) {
    uint8_t tweak[16] = {};
    for (int i = 0; i < 8; ++i) tweak[i] = static_cast<uint8_t>(sector >> (8 * i));
    crypto::xtsAes256Crypt(key64, tweak, sec, sec, 512, enc);
}

void xtsSector(const uint8_t* key, size_t keyBytes, uint64_t sector, uint8_t* sec, bool enc) {
    if (keyBytes == 64) xts256Sector(key, sector, sec, enc);
    else xts128Sector(key, sector, sec, enc);
}

bool cstrField(const uint8_t* p, size_t n, const char* want) {
    const size_t w = std::strlen(want);
    if (w >= n) return false;
    return std::memcmp(p, want, w) == 0 && p[w] == 0;
}

} // namespace

bool looksLikeLuks1(const uint8_t* p, size_t n) {
    return p && n >= 8 && std::memcmp(p, kMagic, 6) == 0 && rdBe16(p + 6) == 1;
}

bool applyLuksMasterKey(DiskReader& reader, const LuksUnlockResult& unlocked) {
    if (!unlocked.success || unlocked.masterKey.empty()) return false;
    if (!reader.setXtsFvek(unlocked.masterKey.data(), unlocked.masterKey.size())) return false;
    reader.setXtsDecryptFrom(unlocked.payloadOffsetBytes);
    return true;
}

LuksUnlockResult unlockLuks1WithPassword(DiskReader& reader, const std::string& passwordUtf8,
                                         uint64_t volumeOffsetBytes) {
    LuksUnlockResult r;
    std::vector<uint8_t> hdr(kPhdr, 0);
    if (!readComplete(reader.readBytes(volumeOffsetBytes, kPhdr, hdr.data()), kPhdr) ||
        !looksLikeLuks1(hdr.data(), hdr.size())) {
        r.error = "not LUKS1";
        if (volumeOffsetBytes != 0) return r;
        PartitionScanner ps(&reader);
        auto gpt = ps.parseGPT();
        auto mbr = ps.parseMBR();
        auto apm = ps.parseAPM();
        const auto& parts = !gpt.empty() ? gpt : (!mbr.empty() ? mbr : apm);
        const uint32_t ss = reader.getSectorSize() ? reader.getSectorSize() : 512;
        for (const auto& p : parts) {
            if (p.startSector == 0 || p.sizeInSectors == 0) continue;
            auto u = unlockLuks1WithPassword(reader, passwordUtf8,
                                             static_cast<uint64_t>(p.startSector) * ss);
            if (u.success) return u;
        }
        return r;
    }
    if (!cstrField(hdr.data() + 8, 32, "aes") || !cstrField(hdr.data() + 40, 32, "xts-plain64") ||
        !cstrField(hdr.data() + 72, 32, "sha256")) {
        r.error = "LUKS cipher/hash not aes-xts-plain64/sha256";
        return r;
    }
    const uint32_t payloadSec = rdBe32(hdr.data() + 104);
    const uint32_t keyBytes = rdBe32(hdr.data() + 108);
    if ((keyBytes != 32 && keyBytes != 64) || payloadSec == 0) {
        r.error = "invalid LUKS key size";
        return r;
    }
    const uint8_t* mkDig = hdr.data() + 112;
    const uint8_t* mkSalt = hdr.data() + 132;
    const uint32_t mkIter = rdBe32(hdr.data() + 164);
    if (mkIter == 0 || mkIter > 5000000) {
        r.error = "invalid LUKS digest iterations";
        return r;
    }
    const uint8_t* slots = hdr.data() + 208;
    for (uint32_t s = 0; s < kSlots; ++s) {
        const uint8_t* kb = slots + s * 48;
        if (rdBe32(kb) != kKeyEnabled) continue;
        const uint32_t pwdIter = rdBe32(kb + 4);
        const uint8_t* salt = kb + 8;
        const uint32_t kmSec = rdBe32(kb + 40);
        const uint32_t stripes = rdBe32(kb + 44);
        if (pwdIter == 0 || pwdIter > 5000000 || stripes < 2 || stripes > 4000) continue;
        const uint64_t afLen = static_cast<uint64_t>(keyBytes) * stripes;
        const uint64_t afSecs = (afLen + 511) / 512;
        if (afSecs == 0 || afSecs > 4096) continue;
        std::vector<uint8_t> pwdKey(keyBytes);
        crypto::pbkdf2HmacSha256(reinterpret_cast<const uint8_t*>(passwordUtf8.data()),
                                 passwordUtf8.size(), salt, kSalt, pwdIter, pwdKey.data(), keyBytes);
        std::vector<uint8_t> af(static_cast<size_t>(afSecs * 512), 0);
        const uint64_t kmOff = volumeOffsetBytes + static_cast<uint64_t>(kmSec) * 512;
        if (!readComplete(reader.readBytes(kmOff, static_cast<uint32_t>(af.size()), af.data()),
                          static_cast<uint32_t>(af.size())))
            continue;
        for (uint64_t i = 0; i < afSecs; ++i)
            xtsSector(pwdKey.data(), keyBytes, static_cast<uint64_t>(kmSec) + i, af.data() + i * 512, false);
        std::vector<uint8_t> mk(keyBytes);
        afMerge(af.data(), mk.data(), keyBytes, stripes);
        uint8_t dig[kDigest];
        crypto::pbkdf2HmacSha256(mk.data(), mk.size(), mkSalt, kSalt, mkIter, dig, kDigest);
        if (std::memcmp(dig, mkDig, kDigest) != 0) continue;
        r.success = true;
        r.masterKey = std::move(mk);
        r.payloadOffsetBytes = volumeOffsetBytes + static_cast<uint64_t>(payloadSec) * 512;
        return r;
    }
    if (r.error.empty()) r.error = "LUKS password did not open a keyslot";
    return r;
}

std::vector<uint8_t> wrapLuks1(const std::vector<uint8_t>& payload, const std::string& passwordUtf8,
                               uint32_t keyBytes) {
    if (keyBytes != 32 && keyBytes != 64) return {};
    constexpr uint32_t kStripes = 4;
    constexpr uint32_t kIter = 2;
    constexpr uint32_t kKmSec = 8;
    constexpr uint32_t kPaySec = 32;
    const uint32_t payBytes = static_cast<uint32_t>(((payload.size() + 511) / 512) * 512);
    std::vector<uint8_t> img((static_cast<size_t>(kPaySec) * 512) + payBytes, 0);
    uint8_t* h = img.data();
    std::memcpy(h, kMagic, 6);
    wrBe16(h + 6, 1);
    std::memcpy(h + 8, "aes", 4);
    std::memcpy(h + 40, "xts-plain64", 12);
    std::memcpy(h + 72, "sha256", 7);
    wrBe32(h + 104, kPaySec);
    wrBe32(h + 108, keyBytes);
    std::vector<uint8_t> mk(keyBytes);
    for (uint32_t i = 0; i < keyBytes; ++i) mk[i] = static_cast<uint8_t>(0xA0 + i);
    uint8_t mkSalt[kSalt];
    uint8_t slotSalt[kSalt];
    std::memset(mkSalt, 0x11, kSalt);
    std::memset(slotSalt, 0x22, kSalt);
    crypto::pbkdf2HmacSha256(mk.data(), keyBytes, mkSalt, kSalt, kIter, h + 112, kDigest);
    std::memcpy(h + 132, mkSalt, kSalt);
    wrBe32(h + 164, kIter);
    std::memcpy(h + 168, "00000000-0000-0000-0000-000000000001", 36);
    uint8_t* slots = h + 208;
    for (uint32_t s = 0; s < kSlots; ++s) wrBe32(slots + s * 48, kKeyDisabled);
    uint8_t* kb = slots;
    wrBe32(kb, kKeyEnabled);
    wrBe32(kb + 4, kIter);
    std::memcpy(kb + 8, slotSalt, kSalt);
    wrBe32(kb + 40, kKmSec);
    wrBe32(kb + 44, kStripes);
    std::vector<uint8_t> pwdKey(keyBytes);
    crypto::pbkdf2HmacSha256(reinterpret_cast<const uint8_t*>(passwordUtf8.data()), passwordUtf8.size(),
                             slotSalt, kSalt, kIter, pwdKey.data(), keyBytes);
    std::vector<uint8_t> af(static_cast<size_t>(keyBytes) * kStripes, 0);
    afSplitZeros(mk.data(), af.data(), keyBytes, kStripes);
    uint8_t sec[512];
    std::memset(sec, 0, 512);
    std::memcpy(sec, af.data(), af.size());
    xtsSector(pwdKey.data(), keyBytes, kKmSec, sec, true);
    std::memcpy(img.data() + kKmSec * 512, sec, 512);
    for (size_t off = 0; off < payload.size(); off += 512) {
        std::memset(sec, 0, 512);
        const size_t n = std::min(payload.size() - off, static_cast<size_t>(512));
        std::memcpy(sec, payload.data() + off, n);
        xtsSector(mk.data(), keyBytes, static_cast<uint64_t>(off / 512), sec, true);
        std::memcpy(img.data() + kPaySec * 512 + off, sec, 512);
    }
    return img;
}

} // namespace byteback
