#include "crypto/byteback_aes_ccm.h"
#include "crypto/byteback_aes.h"
#include <algorithm>
#include <cstring>

// RFC 3610 CCM (counter with CBC-MAC). AR2 math audit found the previous
// implementation used a nonsense flags byte (0x0D: internally L=14 vs a
// 2-byte length field) and computed the CBC-MAC over the CIPHERTEXT — either
// alone makes BitLocker VMK/FVEK unwrapping impossible on real disks. This
// version is generic over nonce length and MAC size:
//   L    = 15 - nonceLen                     (BitLocker: 12-byte nonce → L=3)
//   B0   = ((M-2)/2)<<3 | (L-1) | (aadata? — none)  (BitLocker/M=16: 0x3A)
//   ctr  = (L-1) | nonce | big-endian counter, starting at 1 (0x02 | ...)
//   MAC  = CBC-MAC over the PLAINTEXT (S0 XORed in), tag appended/trailing.

namespace byteback {
namespace crypto {

namespace {

constexpr size_t kBlock = 16;

void xorBlock(uint8_t out[kBlock], const uint8_t a[kBlock], const uint8_t b[kBlock]) {
    for (int i = 0; i < kBlock; ++i) out[i] = static_cast<uint8_t>(a[i] ^ b[i]);
}

// CBC-MAC over `data` (no AAD support — BitLocker blobs carry none).
void ccmMac(const uint8_t key[32], const uint8_t nonce[12], size_t tagLen,
            const uint8_t* data, size_t dataLen, uint8_t tagOut[16]) {
    uint8_t b0[kBlock], mac[kBlock];
    ccmFormatB0(b0, nonce, 12, tagLen, dataLen);
    aes256EncryptBlock(key, b0, mac);
    for (size_t off = 0; off < dataLen; off += kBlock) {
        uint8_t block[kBlock] = {};
        const size_t take = std::min<size_t>(kBlock, dataLen - off);
        std::memcpy(block, data + off, take);
        xorBlock(mac, mac, block);
        aes256EncryptBlock(key, mac, mac);
    }
    std::memcpy(tagOut, mac, kBlock);
}

} // namespace

// Wire-format builders (header-declared for the AR2 format tests).
void ccmFormatB0(uint8_t b0[16], const uint8_t* nonce, size_t nonceLen,
                 size_t tagLen, size_t ptLen) {
    const size_t L = 15 - nonceLen;
    std::memset(b0, 0, 16);
    b0[0] = static_cast<uint8_t>((((tagLen - 2) / 2) << 3) | (L - 1));
    std::memcpy(b0 + 1, nonce, nonceLen);
    // Message length, big-endian, in the trailing L bytes.
    for (size_t i = 0; i < L; ++i) {
        b0[16 - 1 - i] = static_cast<uint8_t>((ptLen >> (8 * i)) & 0xFF);
    }
}

void ccmFormatCounter(uint8_t ctr[16], const uint8_t* nonce, size_t nonceLen, uint64_t counter) {
    const size_t L = 15 - nonceLen;
    std::memset(ctr, 0, 16);
    ctr[0] = static_cast<uint8_t>(L - 1);
    std::memcpy(ctr + 1, nonce, nonceLen);
    for (size_t i = 0; i < L; ++i) {
        ctr[16 - 1 - i] = static_cast<uint8_t>((counter >> (8 * i)) & 0xFF);
    }
}

size_t aesCcmCiphertextLen(size_t ptLen) { return ptLen + 16; }
size_t aesCcmPlaintextLen(size_t ctLen) { return (ctLen >= 16) ? ctLen - 16 : 0; }

bool aesCcmEncrypt(const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t* pt, size_t ptLen, uint8_t* ct, size_t ctLen) {
    if (!key || !nonce || (!pt && ptLen) || (!ct && ctLen)) return false;
    if (ctLen != ptLen + 16) return false;

    // MAC over the plaintext.
    uint8_t s0[kBlock], tag[kBlock];
    ccmMac(key, nonce, 16, pt, ptLen, tag);

    // Keystream: S0 masks the tag, S1.. mask the payload.
    uint8_t ctr[kBlock], ks[kBlock];
    ccmFormatCounter(ctr, nonce, 12, 0);
    aes256EncryptBlock(key, ctr, s0);
    uint64_t counter = 1;
    for (size_t off = 0; off < ptLen; off += kBlock) {
        ccmFormatCounter(ctr, nonce, 12, counter++);
        aes256EncryptBlock(key, ctr, ks);
        const size_t take = std::min<size_t>(kBlock, ptLen - off);
        for (size_t i = 0; i < take; ++i) {
            ct[off + i] = static_cast<uint8_t>(pt[off + i] ^ ks[i]);
        }
    }
    for (int i = 0; i < 16; ++i) ct[ptLen + i] = static_cast<uint8_t>(tag[i] ^ s0[i]);
    return true;
}

bool aesCcmDecrypt(const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t* ct, size_t ctLen, uint8_t* pt, size_t ptLen) {
    if (!key || !nonce || !ct || !pt || ctLen < 16 || ptLen + 16 != ctLen) return false;

    const uint8_t* tag = ct + ptLen; // trailing 16-byte tag
    uint8_t s0[kBlock], ctr[kBlock], ks[kBlock];

    // Decrypt payload first (RFC 3610 §2.6: the MAC is over the PLAINTEXT).
    ccmFormatCounter(ctr, nonce, 12, 0);
    aes256EncryptBlock(key, ctr, s0);
    uint64_t counter = 1;
    for (size_t off = 0; off < ptLen; off += kBlock) {
        ccmFormatCounter(ctr, nonce, 12, counter++);
        aes256EncryptBlock(key, ctr, ks);
        const size_t take = std::min<size_t>(kBlock, ptLen - off);
        for (size_t i = 0; i < take; ++i) {
            pt[off + i] = static_cast<uint8_t>(ct[off + i] ^ ks[i]);
        }
    }

    uint8_t computed[kBlock];
    ccmMac(key, nonce, 16, pt, ptLen, computed);
    uint8_t expected[kBlock];
    for (int i = 0; i < 16; ++i) expected[i] = static_cast<uint8_t>(tag[i] ^ s0[i]);

    // Constant-time compare — key-material authentication must not leak.
    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) diff |= static_cast<uint8_t>(computed[i] ^ expected[i]);
    return diff == 0;
}

} // namespace crypto
} // namespace byteback
