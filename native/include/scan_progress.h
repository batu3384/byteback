#pragma once

#include <cstdint>
#include <algorithm>
#include <atomic>
#include <numeric>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

namespace byteback {

// Scan UI must never rewind. File startSector is not disk-scan order.
struct MonotonicMeter {
    uint64_t last = 0;
    uint64_t tick(uint64_t proposed) {
        if (proposed < last) proposed = last;
        last = proposed;
        return last;
    }
};

// (a * b) / d without wrapping on multi-TB sector counts.
inline uint64_t mulDivU64(uint64_t a, uint64_t b, uint64_t d) {
    if (d == 0 || a == 0 || b == 0) return 0;
    uint64_t g = std::gcd(a, d);
    a /= g;
    d /= g;
    g = std::gcd(b, d);
    b /= g;
    d /= g;
    if (d == 0) return 0;
    if (b != 0 && a <= UINT64_MAX / b) return (a * b) / d;

#if defined(_MSC_VER) && defined(_M_X64)
    unsigned long long hi = 0;
    unsigned long long lo = _umul128(a, b, &hi);
    if (hi >= d) return ~uint64_t{0};
    unsigned long long rem = 0;
    return _udiv128(hi, lo, d, &rem);
#elif defined(__SIZEOF_INT128__)
    return static_cast<uint64_t>((static_cast<unsigned __int128>(a) * b) / d);
#else
    return ~uint64_t{0};
#endif
}

// Map discrete work into [0, budget). Stays below budget so the caller can
// send budget as "complete" once. estimatedTotal 0 is treated as 1.
inline uint64_t mapWorkToBudget(uint64_t done, uint64_t estimatedTotal, uint64_t budget) {
    if (budget == 0) return 0;
    if (estimatedTotal == 0) estimatedTotal = 1;
    if (done >= estimatedTotal) return budget - 1;
    return mulDivU64(done, budget - 1, estimatedTotal);
}

// Process-wide: carve workers must not read a thread_local leftover "metadata".
extern std::atomic<const char*> g_scanPhase;

} // namespace byteback
