#pragma once

#include "byteback_fs.h"
#include "byteback_io.h"
#include <atomic>
#include <cstdint>

namespace byteback {

// Linear metadata-page probe when the checkpoint walk emits nothing.
// 16384 clusters ≈ 64 MiB at 4 KiB. g_refsLinearProbeClusters > 0 overrides (tests).
inline constexpr uint64_t kRefsLinearProbeClustersDefault = 16384;
extern std::atomic<uint64_t> g_refsLinearProbeClusters;

class RefsParser : public FileSystemParser {
public:
    RefsParser();
    ~RefsParser() override;

    bool scan(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning = nullptr) override;

    bool scanAt(DiskReader& reader, FileRecordCallback callback, std::atomic<bool>* isRunning,
                uint64_t partitionOffsetBytes, uint64_t partitionSizeBytes = 0);
};

// Boot VBR + SUPB at cluster 30. Directory listing is a ministore B+-tree walk
// when checkpoint refs resolve; otherwise a metadata-page scan for entry records.
// Integrity: optional CRC64-ECMA trailer on entry records; SUPB self-check at +48.
inline constexpr const char* kRefsSupbUnreadPath = "/refs-supb-unread/";
inline constexpr const char* kRefsSupbUnreadSource = "refs_supb_unread";
inline constexpr const char* kRefsPageUnreadPath = "/refs-page-unread/";
inline constexpr const char* kRefsPageUnreadSource = "refs_page_unread";

bool probeRefsBoot(const uint8_t* boot, size_t bootLen, uint32_t& bytesPerSector,
                   uint32_t& sectorsPerCluster);

} // namespace byteback
