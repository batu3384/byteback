#pragma once

#include "byteback_db.h"
#include "io/volume_mapper_win.h"
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace byteback {

// e2e seedScanFixture: JS Number → FileRecord.mftRef. Absent/NaN/negative stay -1.
inline int64_t parseSeedMftRef(bool present, double value) {
    if (!present) return -1;
    if (!std::isfinite(value) || value < 0.0 ||
        value > static_cast<double>(std::numeric_limits<int64_t>::max())) {
        return -1;
    }
    return static_cast<int64_t>(value);
}

inline void appendSeedRun(FileRecord& r, double start, double count) {
    if (!std::isfinite(start) || !std::isfinite(count) || start < 0.0 || count <= 0.0) return;
    r.runs.push_back({static_cast<uint64_t>(start), static_cast<uint64_t>(count)});
}

inline bool isSeedEvidenceImagePath(const std::string& p) {
    return isEvidenceImagePath(p);
}

} // namespace byteback
