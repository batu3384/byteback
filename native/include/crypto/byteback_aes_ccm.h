#pragma once

#include <cstddef>
#include <cstdint>

namespace byteback {
namespace crypto {

// RFC 3610 CCM over AES-256, shaped for BitLocker key blobs:
// 12-byte nonce, 16-byte tag (M=16) appended AFTER the ciphertext, L=3.
// Flags follow RFC 3610: B0 = ((M-2)/2)<<3 | (L-1) = 0x3A, counters = L-1 =
// 0x02, CBC-MAC over the PLAINTEXT. The previous implementation's 0x0D flags
// and MAC-over-ciphertext could never unwrap a real BitLocker key blob.

bool aesCcmDecrypt(const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t* ct, size_t ctLen, uint8_t* pt, size_t ptLen);

bool aesCcmEncrypt(const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t* pt, size_t ptLen, uint8_t* ct, size_t ctLen);

size_t aesCcmCiphertextLen(size_t ptLen);
size_t aesCcmPlaintextLen(size_t ctLen);

// Test hooks: exact wire-format builders (AR2 math audit pins these).
void ccmFormatB0(uint8_t b0[16], const uint8_t* nonce, size_t nonceLen,
                 size_t tagLen, size_t ptLen);
void ccmFormatCounter(uint8_t ctr[16], const uint8_t* nonce, size_t nonceLen, uint64_t counter);

} // namespace crypto
} // namespace byteback
