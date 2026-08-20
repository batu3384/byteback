#include "crypto/wolf_aes_ccm.h"
#include "crypto/wolf_aes.h"
#include <algorithm>
#include <cstring>

namespace wolf {
namespace crypto {

namespace {

void xorBlock(uint8_t out[16], const uint8_t a[16], const uint8_t b[16]) {
    for (int i = 0; i < 16; ++i) out[i] = static_cast<uint8_t>(a[i] ^ b[i]);
}

void ccmFormatB0(uint8_t b0[16], const uint8_t nonce[12], size_t ptLen) {
    b0[0] = static_cast<uint8_t>(14 - 1); // L=2, M=16 → flags 0x0D
    std::memcpy(b0 + 1, nonce, 12);
    b0[14] = static_cast<uint8_t>((ptLen >> 8) & 0xFF);
    b0[15] = static_cast<uint8_t>(ptLen & 0xFF);
}

void ccmCounter(uint8_t ctr[16], const uint8_t nonce[12], uint32_t i) {
    ctr[0] = static_cast<uint8_t>(14 - 1);
    std::memcpy(ctr + 1, nonce, 12);
    ctr[14] = static_cast<uint8_t>((i >> 8) & 0xFF);
    ctr[15] = static_cast<uint8_t>(i & 0xFF);
}

} // namespace

bool aesCcmDecrypt(const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t* ct, size_t ctLen, uint8_t* pt, size_t ptLen) {
    if (!key || !nonce || !ct || !pt || ctLen < 16 || ptLen + 16 != ctLen) return false;

    const uint8_t* tag = ct + ctLen - 16;
    const uint8_t* enc = ct;
    size_t encLen = ctLen - 16;
    if (encLen != ptLen) return false;

    uint8_t b0[16], s0[16], x[16], ctr[16], t[16];
    ccmFormatB0(b0, nonce, ptLen);
    aes256EncryptBlock(key, b0, s0);

    std::memset(x, 0, 16);
    xorBlock(x, x, s0);

    for (size_t off = 0; off < encLen; off += 16) {
        uint8_t block[16] = {};
        size_t take = std::min<size_t>(16, encLen - off);
        std::memcpy(block, enc + off, take);
        xorBlock(x, x, block);
        aes256EncryptBlock(key, x, x);
    }

    ccmCounter(ctr, nonce, 0);
    aes256EncryptBlock(key, ctr, t);
    for (int i = 0; i < 16; ++i) {
        if ((t[i] ^ x[i]) != tag[i]) return false;
    }

    uint32_t counter = 1;
    for (size_t off = 0; off < ptLen; off += 16) {
        ccmCounter(ctr, nonce, counter++);
        aes256EncryptBlock(key, ctr, t);
        size_t take = std::min<size_t>(16, ptLen - off);
        for (size_t i = 0; i < take; ++i) {
            pt[off + i] = static_cast<uint8_t>(enc[off + i] ^ t[i]);
        }
    }
    return true;
}

} // namespace crypto
} // namespace wolf
