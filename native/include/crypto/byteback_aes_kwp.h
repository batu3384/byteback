#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace byteback {
namespace crypto {

// RFC 3394 AES Key Wrap. kekLen: 16 (AES-128) or 32 (AES-256).
bool aesKwpUnwrap(const uint8_t* kek, size_t kekLen,
                  const uint8_t* wrapped, size_t wrappedLen,
                  uint8_t* out, size_t outCap, size_t* outLen);
bool aesKwpWrap(const uint8_t* kek, size_t kekLen,
                const uint8_t* plain, size_t plainLen,
                uint8_t* out, size_t outCap, size_t* outLen);

} // namespace crypto
} // namespace byteback
