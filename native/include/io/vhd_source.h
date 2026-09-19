#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace byteback {

// Connectix VHD, VHDX (parent locator relative path), sparse/zlib VMDK, VDI, QCOW2, UDIF DMG.
// Expand is in-memory (ponytail: ByteSource if virtual disks grow past 64 MiB).
std::vector<uint8_t> wrapFixedVhd(const std::vector<uint8_t>& payload);
std::vector<uint8_t> wrapDynamicVhd(const std::vector<uint8_t>& payload,
                                    const std::string& parentRelative = {});
std::vector<uint8_t> wrapVhdx(const std::vector<uint8_t>& payload, bool hasParent = false,
                              const std::string& parentRelative = {});
std::vector<uint8_t> wrapVmdk(const std::vector<uint8_t>& payload, bool hasParent = false,
                              bool compressed = false, const std::string& parentRelative = {});
std::vector<uint8_t> wrapVdi(const std::vector<uint8_t>& payload, bool differencing = false);
std::vector<uint8_t> wrapQcow2(const std::vector<uint8_t>& payload, bool hasBacking = false,
                               bool compressed = false, const std::string& backingRelative = {});
std::vector<uint8_t> wrapDmg(const std::vector<uint8_t>& payload, uint32_t blockType = 1,
                             bool encrypted = false);
bool extractVhdPayload(const uint8_t* image, size_t n, std::vector<uint8_t>& payload,
                       std::string& err, const std::string& imagePath = {});
bool extractVhdFile(const std::string& path, std::vector<uint8_t>& payload, std::string& err);
inline bool extractFixedVhdPayload(const uint8_t* image, size_t n, std::vector<uint8_t>& payload,
                                   std::string& err) {
    return extractVhdPayload(image, n, payload, err);
}

} // namespace byteback
