// AES Key Wrap (RFC 3394) — unwrap only. Needed for FileVault2 VE-KEK
// unwrapping (APFS keybag) and any future AES-KW key blobs.
#include "crypto/byteback_aes.h"
#include "crypto/byteback_aes_kwp.h"
#include <cstring>

namespace byteback {
namespace crypto {

static void aesBlockEnc(const uint8_t* key, size_t keyLen, const uint8_t in[16], uint8_t out[16]) {
    if (keyLen == 16) aes128EncryptBlock(key, in, out);
    else aes256EncryptBlock(key, in, out);
}
static void aesBlockDec(const uint8_t* key, size_t keyLen, const uint8_t in[16], uint8_t out[16]) {
    if (keyLen == 16) aes128DecryptBlock(key, in, out);
    else aes256DecryptBlock(key, in, out);
}

bool aesKwpUnwrap(const uint8_t* kek, size_t kekLen,
                  const uint8_t* wrapped, size_t wrappedLen,
                  uint8_t* out, size_t outCap, size_t* outLen) {
    if (!kek || !wrapped || !out || !outLen) return false;
    if (kekLen != 16 && kekLen != 32) return false;
    if (wrappedLen < 24 || (wrappedLen % 8) != 0) return false;
    const size_t n = wrappedLen / 8 - 1;
    if (outCap < n * 8) return false;

    uint8_t a[16];
    std::memcpy(a, wrapped, 8);
    std::memset(a + 8, 0, 8);
    std::vector<uint8_t> r((n + 1) * 8);
    std::memcpy(r.data() + 8, wrapped + 8, n * 8);

    for (int j = 5; j >= 0; --j) {
        for (size_t i = n; i >= 1; --i) {
            uint8_t b[16];
            uint64_t t = static_cast<uint64_t>(n * static_cast<size_t>(j) + i);
            uint8_t ax[8];
            std::memcpy(ax, a, 8);
            for (int k = 0; k < 8; ++k) ax[7 - k] ^= static_cast<uint8_t>(t >> (8 * k));
            std::memcpy(b, ax, 8);
            std::memcpy(b + 8, r.data() + i * 8, 8);
            uint8_t dec[16];
            aesBlockDec(kek, kekLen, b, dec);
            std::memcpy(a, dec, 8);
            std::memcpy(r.data() + i * 8, dec + 8, 8);
        }
    }
    static const uint8_t kIv[8] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};
    if (std::memcmp(a, kIv, 8) != 0) return false;
    std::memcpy(out, r.data() + 8, n * 8);
    *outLen = n * 8;
    return true;
}

bool aesKwpWrap(const uint8_t* kek, size_t kekLen,
                const uint8_t* plain, size_t plainLen,
                uint8_t* out, size_t outCap, size_t* outLen) {
    if (!kek || !plain || !out || !outLen) return false;
    if (kekLen != 16 && kekLen != 32) return false;
    if (plainLen < 16 || (plainLen % 8) != 0) return false;
    const size_t n = plainLen / 8;
    if (outCap < (n + 1) * 8) return false;

    uint8_t a[16];
    static const uint8_t kIv[8] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};
    std::memcpy(a, kIv, 8);
    std::vector<uint8_t> r((n + 1) * 8);
    std::memcpy(r.data() + 8, plain, n * 8);

    for (int j = 0; j <= 5; ++j) {
        for (size_t i = 1; i <= n; ++i) {
            uint8_t b[16];
            std::memcpy(b, a, 8);
            std::memcpy(b + 8, r.data() + i * 8, 8);
            uint8_t enc[16];
            aesBlockEnc(kek, kekLen, b, enc);
            uint64_t t = static_cast<uint64_t>(n * static_cast<size_t>(j) + i);
            for (int k = 0; k < 8; ++k) enc[7 - k] ^= static_cast<uint8_t>(t >> (8 * k));
            std::memcpy(a, enc, 8);
            std::memcpy(r.data() + i * 8, enc + 8, 8);
        }
    }
    std::memcpy(out, a, 8);
    std::memcpy(out + 8, r.data() + 8, n * 8);
    *outLen = (n + 1) * 8;
    return true;
}

} // namespace crypto
} // namespace byteback
