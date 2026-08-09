#include "wolf_carver.h"
#include <cmath>

namespace wolf {

double EntropyAnalyzer::calculateShannonEntropy(const std::vector<uint8_t>& buffer, size_t offset, size_t length) {
    if (length == 0 || offset + length > buffer.size()) return 0.0;

    size_t counts[256] = {0};
    for (size_t i = offset; i < offset + length; ++i) {
        counts[buffer[i]]++;
    }

    double entropy = 0.0;
    for (int i = 0; i < 256; ++i) {
        if (counts[i] > 0) {
            double p = static_cast<double>(counts[i]) / length;
            entropy -= p * log2(p);
        }
    }

    return entropy;
}

} // namespace wolf
