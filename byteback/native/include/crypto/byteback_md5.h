#pragma once

// Shared MD5 implementation. Originally hand-rolled inside recovery_engine.cpp;
// extracted to a dedicated module so the EWF/E01 imager can compute image
// digests with the exact same code the recovery path uses. (MD5 is retained
// because EWF embeds an MD5 digest by format definition; SHA-256 is used
// alongside it where we control the output.)
//
// ponytail: using our own MD5; upgrade path is OpenSSL/BoringSSL EVP.

#include <cstdint>
#include <cstddef>
#include <string>

namespace byteback {
namespace crypto {

class Md5 {
public:
    Md5();
    void update(const uint8_t* data, size_t len);
    // Finalize and return the 32-char lowercase hex digest.
    std::string finalHex();
    // Finalize and write the 16 raw digest bytes.
    void finalRaw(uint8_t out[16]);

private:
    void transform(const uint8_t block[64]);

    uint32_t state_[4];
    uint64_t count_;
    uint8_t buffer_[64];
};

// One-shot helper.
std::string md5Hex(const uint8_t* data, size_t len);

} // namespace crypto
} // namespace byteback
