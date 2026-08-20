#pragma once

#include <cstddef>
#include <cstdint>

namespace wolf {

enum class RefsChecksumType : uint8_t {
    None = 0,
    Crc32C = 1,
    Crc64Ecma = 2,
};

// ReFS metadata checksum algorithms (types 1 and 2).
uint32_t refsCrc32c(const uint8_t* data, size_t len);
uint64_t refsCrc64Ecma(const uint8_t* data, size_t len);

// Range-explicit verifier — caller picks [start,end); ReFS block coverage is not guessed.
bool verifyRefsChecksum(RefsChecksumType type, const uint8_t* data, size_t len,
                        size_t start, size_t end, uint64_t stored);

// ponytail: optional self-check slot at page+48 (64-bit LE stored CRC64 over [0,48)).
// Returns false when no checksum slot present; otherwise sets confidenceOut.
bool tryVerifyRefsMetadataPage(const uint8_t* page, size_t pageSize, int& confidenceOut);

} // namespace wolf
