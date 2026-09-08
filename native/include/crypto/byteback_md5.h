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

// Serializable MD5 midstream state (imaging resume: the digest continues from
// where a cancelled run stopped). bufLen is the number of valid bytes in
// `buffer`; it always equals count % 64 and is stored so a corrupted blob is
// detectable instead of silently desynchronizing the stream.
struct Md5State {
    uint32_t state[4];
    uint64_t count;
    uint8_t buffer[64];
    uint32_t bufLen;
};

class Md5 {
public:
    Md5();
    void update(const uint8_t* data, size_t len);
    // Finalize and return the 32-char lowercase hex digest.
    std::string finalHex();
    // Finalize and write the 16 raw digest bytes.
    void finalRaw(uint8_t out[16]);

    // Snapshot the midstream state. Fails (returns false, out untouched) only
    // after finalHex/finalRaw consumed the context.
    bool saveState(Md5State& out) const;
    // Restore a snapshot produced by saveState. Fails on an inconsistent
    // state (bufLen > 64 or bufLen != count % 64), leaving `this` unchanged.
    bool loadState(const Md5State& s);

private:
    void transform(const uint8_t block[64]);

    uint32_t state_[4];
    uint64_t count_;
    uint8_t buffer_[64];
    bool finalized_ = false;
};

// One-shot helper.
std::string md5Hex(const uint8_t* data, size_t len);

} // namespace crypto
} // namespace byteback
