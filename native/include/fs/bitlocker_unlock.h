#pragma once

#include "byteback_io.h"
#include <cstdint>
#include <string>
#include <vector>

namespace byteback {

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

// Recovery-password stretch: SHA-256 over UTF-16LE of the 48-digit string,
// then 0x100000 further SHA-256 rounds (MS-BITLOCKER / libbde). Input here
// must already be digits-only; unlockBitLockerWithRecoveryPassword strips
// group separators before calling this.
void stretchBitLockerRecoveryKey(const std::string& recoveryDigits, uint8_t out[32]);

// Strip non-digits from a typed recovery password (groups are joined by
// hyphens/spaces in the UI but the KDF hashes the bare digit string).
std::string normalizeBitLockerRecoveryPassword(const std::string& typed);

} // namespace byteback
