#pragma once

// Shared declarations for the NAPI bridge translation units. The bridge is
// split by concern (CA-013): bridge_drives/scan/imager/wipe.cpp own the
// handlers; napi_bridge.cpp keeps only Init()/module registration.

#include <napi.h>
#include "wolf_engine.h"
#include "scan_coordinator.h"
#include "wolf_smart.h"
#include "wolf_imager.h"
#include "wolf_shredder.h"
#include "wolf_recovery.h"
#include "fs/virtual_raid.h"
#include "fs/partition_scanner.h"
#include "forensic/audit_logger.h"
#include <cstdlib>
#include <memory>
#include <exception>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>

#define NAPI_TRY \
    try {

#define NAPI_CATCH \
    } catch (const std::exception& e) { \
        Napi::Error::New(env, e.what()).ThrowAsJavaScriptException(); \
        return env.Undefined(); \
    } catch (...) { \
        Napi::Error::New(env, "Unknown fatal C++ exception").ThrowAsJavaScriptException(); \
        return env.Undefined(); \
    }

struct ScanContext {
    wolf::ScanCoordinator coordinator;
    Napi::ThreadSafeFunction tsfn;
    int64_t scanId = -1;
    std::vector<wolf::FileRecord> fileBuffer;
    std::mutex bufferMutex;
    // CA-007: bad-sector sample list mirrored by the coordinator before each
    // progress tick (written and read on the same worker thread).
    std::vector<uint64_t> badSectors;
};

struct ImagerContext {
    wolf::DiskImager imager;
    Napi::ThreadSafeFunction tsfn;
};

struct BridgeData {
    wolf::Engine engine;
    std::shared_ptr<ScanContext> scanContext;
    std::shared_ptr<ImagerContext> imagerContext;
    // Assembled virtual RAID array (Phase 3). Kept alive here so subsequent
    // scan/imaging calls can read through it; empty until the UI assembles
    // an array via reconstructRaid().
    std::shared_ptr<wolf::VirtualRaid> raid;
    // Path of the hash-chained forensic audit log (set by initDatabase).
    std::string auditLogPath;
};

// bridge_drives.cpp — drives / partitions / raw reads / SMART
Napi::Value GetVersion(const Napi::CallbackInfo& info);
Napi::Value IsAdministrator(const Napi::CallbackInfo& info);
Napi::Value ListDrives(const Napi::CallbackInfo& info);
Napi::Value ListPartitions(const Napi::CallbackInfo& info);
Napi::Value ReadSectors(const Napi::CallbackInfo& info);
Napi::Value GetSmartStatus(const Napi::CallbackInfo& info);

// bridge_scan.cpp — scans / results / timeline / audit log / DB
Napi::Value InitDatabase(const Napi::CallbackInfo& info);
Napi::Value StartScan(const Napi::CallbackInfo& info);
Napi::Value StopScan(const Napi::CallbackInfo& info);
Napi::Value GetFileCount(const Napi::CallbackInfo& info);
Napi::Value GetFilesPage(const Napi::CallbackInfo& info);
Napi::Value GetScanState(const Napi::CallbackInfo& info);
Napi::Value GetLatestScanId(const Napi::CallbackInfo& info);
Napi::Value GetTimelineEvents(const Napi::CallbackInfo& info);
Napi::Value GetAuditLog(const Napi::CallbackInfo& info);

// bridge_imager.cpp
Napi::Value StartImaging(const Napi::CallbackInfo& info);
Napi::Value StopImaging(const Napi::CallbackInfo& info);

// bridge_wipe.cpp — wipe / RAID / recovery
Napi::Value StartWipe(const Napi::CallbackInfo& info);
Napi::Value ReconstructRaid(const Napi::CallbackInfo& info);
Napi::Value RecoverFile(const Napi::CallbackInfo& info);
