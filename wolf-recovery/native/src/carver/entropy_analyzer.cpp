#include "wolf_carver.h"
#include "math/entropy_calculator.h"

namespace wolf {

// Thin adapter: delegates to the single canonical implementation in math/entropy.cpp,
// keeping the offset/length-aware API that callers of EntropyAnalyzer rely on.
double EntropyAnalyzer::calculateShannonEntropy(const uint8_t* buffer, size_t bufferSize, size_t offset, size_t length) {
    if (length == 0 || offset + length > bufferSize) return 0.0;
    return wolf::math::calculateEntropy(buffer + offset, length);
}

} // namespace wolf
