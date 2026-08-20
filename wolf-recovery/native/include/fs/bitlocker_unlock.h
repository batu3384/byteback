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

// Recovery password (clear-key / 0x0800) → VMK → FVEK. TPM/startup-key/password → error.
BitLockerUnlockResult unlockBitLockerWithRecoveryPassword(DiskReader& reader,
                                                        const std::string& passwordUtf8,
                                                        uint64_t volumeOffsetBytes = 0);

} // namespace wolf
