#pragma once

#include <cstddef>
#include <cstdint>

namespace wolf {
namespace crypto {

// AES-128/256 (FIPS-197) + XTS (IEEE 1619 / NIST SP 800-38E).
void aes128EncryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void aes128DecryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void aes256EncryptBlock(const uint8_t key[32], const uint8_t in[16], uint8_t out[16]);
void aes256DecryptBlock(const uint8_t key[32], const uint8_t in[16], uint8_t out[16]);
bool xtsAes128Crypt(const uint8_t key32[32], const uint8_t tweak[16],
                    const uint8_t* in, uint8_t* out, size_t len, bool encrypt);
bool xtsAes256Crypt(const uint8_t key64[64], const uint8_t tweak[16],
                    const uint8_t* in, uint8_t* out, size_t len, bool encrypt);

} // namespace crypto
} // namespace wolf
