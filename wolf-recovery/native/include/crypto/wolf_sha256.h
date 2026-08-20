#pragma once

#include <cstddef>
#include <cstdint>

namespace wolf {
namespace crypto {

void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

} // namespace crypto
} // namespace wolf
