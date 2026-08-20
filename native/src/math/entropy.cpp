#include "math/entropy_calculator.h"
#include <cmath>

namespace byteback {
namespace math {

double calculateEntropy(const uint8_t* data, size_t size) {
    if (size == 0 || data == nullptr) {
        return 0.0;
    }

    size_t counts[256] = {0};
    for (size_t i = 0; i < size; ++i) {
        counts[data[i]]++;
    }

    double entropy = 0.0;
    double invSize = 1.0 / static_cast<double>(size);
    for (int i = 0; i < 256; ++i) {
        if (counts[i] > 0) {
            double p = counts[i] * invSize;
            entropy -= p * std::log2(p);
        }
    }

    return entropy;
}

} // namespace math
} // namespace byteback
