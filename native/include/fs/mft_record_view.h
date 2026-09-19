#pragma once

#include "byteback_io.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace byteback {

struct MftAttrSummary {
    uint32_t type = 0;
    std::string name;
    bool resident = true;
};

struct MftRecordView {
    uint64_t mftRef = 0;
    uint64_t byteOffset = 0;
    std::string signature;
    uint16_t flags = 0;
    std::vector<MftAttrSummary> attrs;
};

// Discovery-only dump of one MFT record. Unread I/O sets *unread and
// returns nullopt — never invents a FILE signature from zeros.
std::optional<MftRecordView> getMftRecordView(DiskReader& reader, uint64_t mftRef,
                                              uint64_t partitionOffsetBytes,
                                              bool* unread);

} // namespace byteback
