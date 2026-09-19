#pragma once

#include "byteback_io.h"
#include <cstdint>
#include <string>
#include <vector>

namespace byteback {

struct LuksUnlockResult {
    bool success = false;
    std::string error;
    std::vector<uint8_t> masterKey; // 32 or 64
    uint64_t payloadOffsetBytes = 0;
};

bool looksLikeLuks1(const uint8_t* p, size_t n);

// Password + LUKS1 PBKDF2-SHA256 + AES-XTS-plain64 128/256. LUKS2/Argon2 out.
LuksUnlockResult unlockLuks1WithPassword(DiskReader& reader, const std::string& passwordUtf8,
                                         uint64_t volumeOffsetBytes = 0);

bool applyLuksMasterKey(DiskReader& reader, const LuksUnlockResult& unlocked);

std::vector<uint8_t> wrapLuks1(const std::vector<uint8_t>& payload, const std::string& passwordUtf8,
                               uint32_t keyBytes = 32);

} // namespace byteback
