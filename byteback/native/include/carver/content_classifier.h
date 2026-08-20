#pragma once

#include "byteback_io.h"
#include "byteback_carver.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace byteback {

// ponytail: entropy + magic heuristics, not ML — refines carve categories.
std::string refineCarveCategory(const uint8_t* data, size_t size, const std::string& extension,
                                const std::string& currentCategory);

} // namespace byteback
