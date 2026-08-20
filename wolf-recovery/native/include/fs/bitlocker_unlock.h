#pragma once

#include "wolf_io.h"
#include <cstdint>
#include <string>
#include <vector>

namespace wolf {

struct BitLockerUnlockResult {
    bool success = false;
    std::string error;
    std::vector<uint8_t> fvek; // 32 or 64 bytes when success
};

// Password protector (0x2000 stretch key + AES-CCM VMK).
BitLockerUnlockResult unlockBitLockerWithPassword(DiskReader& reader,
                                                  const std::string& passwordUtf8,
                                                  uint64_t volumeOffsetBytes = 0);

// Recovery password (0x0800 clear-key) → VMK → FVEK.
BitLockerUnlockResult unlockBitLockerWithRecoveryPassword(DiskReader& reader,
                                                          const std::string& passwordUtf8,
                                                          uint64_t volumeOffsetBytes = 0);

// libbde-compatible password stretch (SHA256×2 + 0x100000 struct iterations).
void deriveBitLockerPasswordKey(const std::string& passwordUtf8, const uint8_t salt[16],
                                uint8_t out[32]);

} // namespace wolf
