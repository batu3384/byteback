#pragma once

#include <cstdint>
#include <cstddef>

namespace byteback {
namespace math {

double calculateEntropy(const uint8_t* data, size_t size);

} // namespace math
} // namespace byteback
