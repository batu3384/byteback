#pragma once

#include <cstddef>
#include <cstdint>

namespace wolf {
namespace crypto {

// AES-CCM decrypt (BitLocker key blobs: 12-byte nonce, 16-byte tag, M=16 L=2).
bool aesCcmDecrypt(const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t* ct, size_t ctLen, uint8_t* pt, size_t ptLen);

} // namespace crypto
} // namespace wolf
