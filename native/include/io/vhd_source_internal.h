#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace byteback {

/** RAM expand cap for dynamic VHD/VHDX attach (honest product limit; not streaming). */
constexpr uint64_t kVhdFamilyMaxExpandBytes = 64ull << 20;

bool extractVhdFile(const std::string& path, std::vector<uint8_t>& payload, std::string& err);
bool extractVhdxPayload(const uint8_t* image, size_t n, std::vector<uint8_t>& payload,
                        std::string& err, const std::string& imagePath);

} // namespace byteback
