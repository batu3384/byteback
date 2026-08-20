// MD5 (RFC 1321) — moved verbatim from recovery_engine.cpp so the recovery
// engine and the EWF imager share one implementation.
#include "crypto/byteback_md5.h"
#include <cstring>
#include <cstdio>

namespace byteback {
namespace crypto {

namespace {
const uint32_t MD5_S[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};
const uint32_t MD5_K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};

inline uint32_t leftRotate(uint32_t x, uint32_t c) { return (x << c) | (x >> (32 - c)); }
} // namespace

Md5::Md5() {
    state_[0] = 0x67452301; state_[1] = 0xefcdab89;
    state_[2] = 0x98badcfe; state_[3] = 0x10325476;
    count_ = 0;
}

void Md5::transform(const uint8_t block[64]) {
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t M[16];
    for (int i = 0; i < 16; i++)
        M[i] = (uint32_t)block[i*4] | ((uint32_t)block[i*4+1]<<8) | ((uint32_t)block[i*4+2]<<16) | ((uint32_t)block[i*4+3]<<24);

    for (uint32_t i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5*i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3*i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7*i) % 16; }
        uint32_t temp = d; d = c; c = b;
        b = b + leftRotate(a + f + MD5_K[i] + M[g], MD5_S[i]);
        a = temp;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
}

void Md5::update(const uint8_t* data, size_t len) {
    size_t index = (size_t)(count_ % 64);
    count_ += len;
    size_t i = 0;
    if (index) {
        size_t part = 64 - index;
        if (len >= part) {
            memcpy(buffer_ + index, data, part);
            transform(buffer_);
            i = part;
        } else {
            memcpy(buffer_ + index, data, len);
            return;
        }
    }
    for (; i + 64 <= len; i += 64)
        transform(data + i);
    if (i < len)
        // After any transform the buffer is empty, so leftover bytes go to
        // its START — the original code wrote to buffer_+i (a data offset),
        // silently corrupting digests for lengths > 64 that did not divide
        // evenly. Caught by round-tripping against Python's hashlib.
        memcpy(buffer_, data + i, len - i);
}

std::string Md5::finalHex() {
    uint8_t raw[16];
    finalRaw(raw);

    char hex[33];
    for (int i = 0; i < 16; ++i) {
        static const char* HEXD = "0123456789abcdef";
        hex[i * 2] = HEXD[raw[i] >> 4];
        hex[i * 2 + 1] = HEXD[raw[i] & 0xF];
    }
    hex[32] = 0;
    return std::string(hex, 32);
}

void Md5::finalRaw(uint8_t out[16]) {
    // Finalize per RFC 1321: append 0x80, zero-pad to 56 mod 64, append the
    // bit length, transform. The original implementation fed the length
    // through update(), which left a full 64-byte buffer untransformed when
    // the length bytes completed the block — producing wrong digests for
    // inputs whose length mod 64 was in [48, 56). Feeding the length bytes
    // directly into the buffer and transforming here closes both that and
    // the update() buffering defect.
    uint64_t bits = count_ * 8;
    size_t index = (size_t)(count_ % 64);
    size_t padLen = (index < 56) ? (56 - index) : (120 - index);

    uint8_t padding[128];
    memset(padding, 0, sizeof(padding));
    padding[0] = 0x80;
    update(padding, padLen);
    // update() has now left the first 56 bytes of the buffer holding the
    // padded message tail (guaranteed by the padding length arithmetic).
    for (int i = 0; i < 8; i++) buffer_[56 + i] = (uint8_t)(bits >> (i * 8));
    transform(buffer_);

    for (int i = 0; i < 4; i++) {
        uint32_t s = state_[i];
        out[i * 4 + 0] = (uint8_t)(s & 0xFF);
        out[i * 4 + 1] = (uint8_t)((s >> 8) & 0xFF);
        out[i * 4 + 2] = (uint8_t)((s >> 16) & 0xFF);
        out[i * 4 + 3] = (uint8_t)((s >> 24) & 0xFF);
    }
}

std::string md5Hex(const uint8_t* data, size_t len) {
    Md5 md5;
    md5.update(data, len);
    return md5.finalHex();
}

} // namespace crypto
} // namespace byteback
