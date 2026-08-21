#pragma once

#include <cstdint>
#include <algorithm>

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

// Map discrete work into [0, budget). Stays below budget so the caller can
// send budget as "complete" once. estimatedTotal 0 is treated as 1.
inline uint64_t mapWorkToBudget(uint64_t done, uint64_t estimatedTotal, uint64_t budget) {
    if (budget == 0) return 0;
    if (estimatedTotal == 0) estimatedTotal = 1;
    if (done >= estimatedTotal) return budget - 1;
    return done * (budget - 1) / estimatedTotal;
}

extern thread_local const char* g_scanPhase;

} // namespace byteback
