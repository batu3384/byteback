#pragma once

#include <cstddef>
#include <cstdint>

namespace byteback {
namespace crypto {

void sha256(const uint8_t* data, size_t len, uint8_t out[32]);
void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen, uint8_t out[32]);
void pbkdf2HmacSha256(const uint8_t* pass, size_t passLen, const uint8_t* salt, size_t saltLen,
                      uint32_t iter, uint8_t* out, size_t outLen);

} // namespace crypto
} // namespace byteback
