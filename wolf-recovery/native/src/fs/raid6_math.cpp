// GF(2^8) arithmetic for RAID 6 Q-syndrome (Reed-Solomon), separated from
// virtual_raid.cpp so the math can be unit-tested without linking the disk
// I/O layer. Field polynomial: 0x11D (x^8 + x^4 + x^3 + x^2 + 1), the same
// field used by ATA/SCSI RAID-6 implementations.
#include "fs/virtual_raid.h"

namespace wolf {
namespace raid6_math {
namespace {
struct GfTables {
    uint8_t exp[512];
    uint16_t log[256];
    GfTables() {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = static_cast<uint16_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
        log[0] = 0xFFFF; // sentinel: log of zero undefined
    }
};
const GfTables& gf() {
    static const GfTables t;
    return t;
}
} // namespace

uint8_t gfMul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return gf().exp[gf().log[a] + gf().log[b]];
}

uint8_t gfPow(int exponent) {
    int e = exponent % 255;
    if (e < 0) e += 255;
    return gf().exp[e];
}

uint8_t gfDiv(uint8_t a, uint8_t b) {
    if (b == 0) throw std::invalid_argument("GF division by zero");
    if (a == 0) return 0;
    return gf().exp[(gf().log[a] + 255 - gf().log[b]) % 255];
}

} // namespace raid6_math
} // namespace wolf
