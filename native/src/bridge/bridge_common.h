#pragma once

// Shared declarations for the NAPI bridge translation units. The bridge is
// split by concern (CA-013): bridge_drives/scan/imager/wipe.cpp own the
// handlers; napi_bridge.cpp keeps only Init()/module registration.

#include <napi.h>
#include "byteback_engine.h"
#include "scan_coordinator.h"
#include "search/content_search.h"
#include "byteback_smart.h"
#include "byteback_imager.h"
#include "byteback_shredder.h"
#include "byteback_recovery.h"
#include "fs/virtual_raid.h"
#include "fs/partition_scanner.h"
#include "scan/dedup_index.h"
#include "forensic/audit_logger.h"
#include "forensic/nsrl_lookup.h"
#include <cstdlib>
#include <memory>
#include <exception>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <utility>

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
    byteback::ScanCoordinator coordinator;
    Napi::ThreadSafeFunction tsfn;
    int64_t scanId = -1;
    std::vector<byteback::FileRecord> fileBuffer;
    byteback::DedupIndex dedupIndex;
    std::mutex bufferMutex;
    // CA-007: bad-sector sample list mirrored by the coordinator before each
    // progress tick (written and read on the same worker thread).
    std::vector<uint64_t> badSectors;
};

struct ImagerContext {
    byteback::DiskImager imager;
    Napi::ThreadSafeFunction tsfn;
};

struct ContentSearchContext {
    byteback::ContentSearchCoordinator coordinator;
    Napi::ThreadSafeFunction tsfn;
};

struct BridgeData {
    byteback::Engine engine;
    std::shared_ptr<ScanContext> scanContext;
    std::shared_ptr<ImagerContext> imagerContext;
    std::shared_ptr<ContentSearchContext> contentSearchContext;
    std::shared_ptr<byteback::VirtualRaid> raid;
    std::string auditLogPath;
    forensic::NsrlLookup nsrl;
    std::atomic<int> heavyOps{0};

    bool tryBeginHeavyOp() {
        int prev = heavyOps.fetch_add(1);
        if (prev > 0) {
            heavyOps.fetch_sub(1);
            return false;
        }
        return true;
    }
    void endHeavyOp() {
        int prev = heavyOps.load();
        while (prev > 0 && !heavyOps.compare_exchange_weak(prev, prev - 1)) {}
    }

    bool diskOpInProgress() const { return heavyOps.load(std::memory_order_acquire) > 0; }
};

inline bool throwIfSharedReaderBusy(Napi::Env env, BridgeData* bdata) {
    if (bdata && bdata->diskOpInProgress()) {
        Napi::Error::New(env, "Another disk operation is already running").ThrowAsJavaScriptException();
        return true;
    }
    return false;
}

// bridge_drives.cpp — drives / partitions / raw reads / SMART
Napi::Value GetVersion(const Napi::CallbackInfo& info);
Napi::Value IsAdministrator(const Napi::CallbackInfo& info);
Napi::Value ListDrives(const Napi::CallbackInfo& info);
Napi::Value ListPartitions(const Napi::CallbackInfo& info);
Napi::Value ReadSectors(const Napi::CallbackInfo& info);
Napi::Value GetSmartStatus(const Napi::CallbackInfo& info);
Napi::Value ResolveVolume(const Napi::CallbackInfo& info);
Napi::Value ListVolumeLetters(const Napi::CallbackInfo& info);

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
Napi::Value SearchFiles(const Napi::CallbackInfo& info);
Napi::Value SearchFileContent(const Napi::CallbackInfo& info);
Napi::Value StartContentSearch(const Napi::CallbackInfo& info);
Napi::Value StopContentSearch(const Napi::CallbackInfo& info);
Napi::Value GetScanSummary(const Napi::CallbackInfo& info);

// bridge_imager.cpp
Napi::Value StartImaging(const Napi::CallbackInfo& info);
Napi::Value StopImaging(const Napi::CallbackInfo& info);

// bridge_wipe.cpp — wipe / RAID / recovery
Napi::Value StartWipe(const Napi::CallbackInfo& info);
Napi::Value SetBitLockerFvek(const Napi::CallbackInfo& info);
Napi::Value SetBitLockerRecoveryPassword(const Napi::CallbackInfo& info);
Napi::Value SetBitLockerPassword(const Napi::CallbackInfo& info);
Napi::Value StartPhysicalWipe(const Napi::CallbackInfo& info);
template<typename Callback>
void tsfnPost(Napi::ThreadSafeFunction& tsfn, Callback&& cb) {
    (void)tsfn.NonBlockingCall(std::forward<Callback>(cb));
}

Napi::Value ReconstructRaid(const Napi::CallbackInfo& info);
Napi::Value FailRaidDisk(const Napi::CallbackInfo& info);
Napi::Value GetRaidState(const Napi::CallbackInfo& info);
Napi::Value RecoverFile(const Napi::CallbackInfo& info);
Napi::Value RecoverFilesBatch(const Napi::CallbackInfo& info);
Napi::Value ReadFilePreview(const Napi::CallbackInfo& info);

// bridge_ops.cpp — case metadata + NSRL
Napi::Value GetCaseInfo(const Napi::CallbackInfo& info);
Napi::Value SetCaseInfo(const Napi::CallbackInfo& info);
Napi::Value LoadNsrl(const Napi::CallbackInfo& info);
Napi::Value LookupNsrl(const Napi::CallbackInfo& info);
Napi::Value GetNsrlStats(const Napi::CallbackInfo& info);
