// bridge_scan.cpp — scan lifecycle, result pages, scan state, unified
// timeline and the audit-log reader. See bridge_common.h for the context.
#include "bridge_common.h"
#include "scan_progress.h"
#include "search/content_search.h"
#include "fs/partition_scanner.h"
#include <atomic>
#include <cstdio>

namespace {
Napi::Array RunsToJs(Napi::Env env, const std::vector<byteback::FileRecord::DataRun>& runs) {
    Napi::Array arr = Napi::Array::New(env, runs.size());
    for (size_t i = 0; i < runs.size(); ++i) {
        Napi::Object runObj = Napi::Object::New(env);
        runObj.Set("startSector", Napi::Number::New(env, static_cast<double>(runs[i].startSector)));
        runObj.Set("sectorCount", Napi::Number::New(env, static_cast<double>(runs[i].sectorCount)));
        arr[i] = runObj;
    }
    return arr;
}

byteback::FileListFilter FilterFromJs(const Napi::Value& v) {
    byteback::FileListFilter f;
    if (!v.IsObject()) return f;
    Napi::Object o = v.As<Napi::Object>();
    if (o.Has("status") && o.Get("status").IsNumber()) {
        f.status = o.Get("status").As<Napi::Number>().Int32Value();
    }
    if (o.Has("category") && o.Get("category").IsString()) {
        f.category = o.Get("category").As<Napi::String>().Utf8Value();
    }
    if (o.Has("query") && o.Get("query").IsString()) {
        f.query = o.Get("query").As<Napi::String>().Utf8Value();
    }
    if (o.Has("sourceLike") && o.Get("sourceLike").IsString()) {
        f.sourceLike = o.Get("sourceLike").As<Napi::String>().Utf8Value();
    }
    if (o.Has("includeDuplicates") && o.Get("includeDuplicates").IsBoolean()) {
        f.includeDuplicates = o.Get("includeDuplicates").As<Napi::Boolean>().Value();
    }
    if (o.Has("includeDiscovery") && o.Get("includeDiscovery").IsBoolean()) {
        f.includeDiscovery = o.Get("includeDiscovery").As<Napi::Boolean>().Value();
    }
    return f;
}

Napi::Object FileRecordToJs(Napi::Env env, const byteback::FileRecord& fr) {
    Napi::Object fileObj = Napi::Object::New(env);
    fileObj.Set("id", Napi::Number::New(env, static_cast<double>(fr.id)));
    fileObj.Set("parentId", Napi::Number::New(env, static_cast<double>(fr.parentId)));
    fileObj.Set("name", jsUtf8(env, fr.name));
    fileObj.Set("extension", jsUtf8(env, fr.extension));
    fileObj.Set("path", jsUtf8(env, fr.path));
    fileObj.Set("sizeBytes", Napi::Number::New(env, static_cast<double>(fr.sizeBytes)));
    fileObj.Set("startSector", Napi::Number::New(env, static_cast<double>(fr.startSector)));
    fileObj.Set("endSector", Napi::Number::New(env, static_cast<double>(fr.endSector)));
    fileObj.Set("status", Napi::Number::New(env, fr.status));
    fileObj.Set("compressed", Napi::Boolean::New(env, fr.compressed));
    fileObj.Set("confidence", Napi::Number::New(env, fr.confidence));
    fileObj.Set("category", jsUtf8(env, fr.category));
    fileObj.Set("source", jsUtf8(env, fr.source));
    fileObj.Set("createdAt", Napi::Number::New(env, static_cast<double>(fr.createdAt)));
    fileObj.Set("modifiedAt", Napi::Number::New(env, static_cast<double>(fr.modifiedAt)));
    fileObj.Set("runs", RunsToJs(env, fr.runs));
    return fileObj;
}

constexpr uint32_t kMaxLiveFileEvents = 0;

void flushScanToDb(BridgeData* bdata, int status) {
    if (!bdata || !bdata->scanContext) return;
    auto context = bdata->scanContext;

    {
        std::lock_guard<std::mutex> lock(context->bufferMutex);
        if (!context->fileBuffer.empty()) {
            bdata->engine.getMetadataStore().insertFilesBatch(context->scanId, context->fileBuffer);
            context->fileBuffer.clear();
        }
    }

    bdata->engine.getMetadataStore().updateScanProgress(
        context->scanId, bdata->engine.getMetadataStore().getScanState(context->scanId).scannedSectors);
    auto st = bdata->engine.getMetadataStore().getScanState(context->scanId);
    int dbStatus = status;
    if (status == 2 && st.totalSectors > 0 && st.scannedSectors > 0 && st.scannedSectors < st.totalSectors) {
        dbStatus = 4;
    }
    bdata->engine.getMetadataStore().completeScan(context->scanId, dbStatus);

    forensic::AuditLogger::GetInstance().LogEvent(
        "SCAN_END | scanId=" + std::to_string(context->scanId) + " | status=" + std::to_string(status));
}

void teardownScanOnJs(BridgeData* bdata, uint64_t generation) {
    if (!bdata || !bdata->scanContext) return;
    if (bdata->scanContext->generation != generation) return;
    bdata->scanContext->tsfn.Release();
    bdata->scanContext.reset();
    bdata->endHeavyOp();
}
} // namespace

Napi::Value InitDatabase(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    byteback::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine || info.Length() < 1) return env.Undefined();

    std::string dbPath = info[0].As<Napi::String>().Utf8Value();
    bool ok = engine->getMetadataStore().open(dbPath);

    // The hash-chained forensic audit log lives next to the metadata DB and
    // records every forensic operation (scan/imaging/wipe/recovery) with a
    // running SHA-256 chain so tampering is detectable.
    if (ok) {
        std::string auditPath = dbPath + ".audit.log";
        forensic::AuditLogger::GetInstance().Initialize(auditPath);
        forensic::AuditLogger::GetInstance().LogEvent("SESSION_START | database=" + dbPath);
        if (bdata) bdata->auditLogPath = auditPath;
        const int64_t orphans = engine->getMetadataStore().reclaimOrphanRunningScans();
        if (orphans > 0) {
            forensic::AuditLogger::GetInstance().LogEvent(
                "SCAN_ORPHAN | count=" + std::to_string(orphans) + " | marked paused");
        }
    }
    return Napi::Boolean::New(env, ok);
    NAPI_CATCH
}

Napi::Value GetFileCount(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    byteback::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine || info.Length() < 1) return env.Undefined();

    int64_t scanId = info[0].As<Napi::Number>().Int64Value();
    byteback::FileListFilter filter;
    if (info.Length() >= 2 && info[1].IsObject()) filter = FilterFromJs(info[1]);
    int64_t count = engine->getMetadataStore().getFileCount(scanId, filter);
    return Napi::Number::New(env, static_cast<double>(count));
    NAPI_CATCH
}

Napi::Value GetFilesPage(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    byteback::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine || info.Length() < 3) return env.Undefined();

    int64_t scanId = info[0].As<Napi::Number>().Int64Value();
    int offset = info[1].As<Napi::Number>().Int32Value();
    int limit = info[2].As<Napi::Number>().Int32Value();
    byteback::FileListFilter filter;
    if (info.Length() >= 4 && info[3].IsObject()) filter = FilterFromJs(info[3]);

    auto files = engine->getMetadataStore().getFiles(scanId, offset, limit, filter);
    Napi::Array result = Napi::Array::New(env, files.size());

    for (size_t i = 0; i < files.size(); ++i) {
        result[i] = FileRecordToJs(env, files[i]);
    }
    return result;
    NAPI_CATCH
}

Napi::Value GetScanState(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    byteback::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine || info.Length() < 1) return env.Undefined();

    int64_t scanId = info[0].As<Napi::Number>().Int64Value();
    auto state = engine->getMetadataStore().getScanState(scanId);

    Napi::Object obj = Napi::Object::New(env);
    obj.Set("id", Napi::Number::New(env, static_cast<double>(state.id)));
    obj.Set("driveIndex", Napi::Number::New(env, state.driveIndex));
    obj.Set("scanType", Napi::String::New(env, state.scanType));
    obj.Set("totalSectors", Napi::Number::New(env, static_cast<double>(state.totalSectors)));
    obj.Set("scannedSectors", Napi::Number::New(env, static_cast<double>(state.scannedSectors)));
    obj.Set("status", Napi::Number::New(env, state.status));
    obj.Set("recoveredFiles", Napi::Number::New(env, static_cast<double>(state.recoveredFiles)));
    obj.Set("metadataComplete", Napi::Boolean::New(env, state.metadataComplete));
    obj.Set("carveResumeSector", Napi::Number::New(env, static_cast<double>(state.carveResumeSector)));
    return obj;
    NAPI_CATCH
}

Napi::Value GetLatestScanId(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return Napi::Number::New(env, -1);
    return Napi::Number::New(env, static_cast<double>(
        bdata->engine.getMetadataStore().getLatestScanId()));
    NAPI_CATCH
}

Napi::Value GetLatestUsableScanId(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return Napi::Number::New(env, -1);
    return Napi::Number::New(env, static_cast<double>(
        bdata->engine.getMetadataStore().getLatestUsableScanId()));
    NAPI_CATCH
}

Napi::Value ResetScanDatabase(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return Napi::Boolean::New(env, false);
    if (bdata->scanContext) {
        Napi::Error::New(env, "Cannot reset while a scan is running").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    return Napi::Boolean::New(env, bdata->engine.getMetadataStore().clearAllScanData());
    NAPI_CATCH
}

Napi::Value GetTimelineEvents(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata || info.Length() < 3 ||
        !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
        Napi::TypeError::New(env, "Expected (scanId, offset, limit, [eventTypeFilter])").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    int64_t scanId = info[0].As<Napi::Number>().Int64Value();
    int offset = info[1].As<Napi::Number>().Int32Value();
    int limit = info[2].As<Napi::Number>().Int32Value();
    std::string filter = (info.Length() >= 4 && info[3].IsString())
                         ? info[3].As<Napi::String>().Utf8Value() : "";

    auto& store = bdata->engine.getMetadataStore();
    auto events = store.getTimelineEvents(scanId, offset, limit, filter);
    int64_t total = store.getTimelineEventCount(scanId, filter);

    Napi::Object out = Napi::Object::New(env);
    out.Set("total", Napi::Number::New(env, static_cast<double>(total)));
    Napi::Array arr = Napi::Array::New(env, events.size());
    for (size_t i = 0; i < events.size(); ++i) {
        Napi::Object e = Napi::Object::New(env);
        e.Set("id", Napi::Number::New(env, static_cast<double>(events[i].id)));
        e.Set("timestamp", Napi::Number::New(env, static_cast<double>(events[i].timestamp)));
        e.Set("eventType", Napi::String::New(env, events[i].eventType));
        e.Set("fileName", Napi::String::New(env, events[i].fileName));
        e.Set("mftRef", Napi::Number::New(env, static_cast<double>(events[i].mftRef)));
        e.Set("source", Napi::String::New(env, events[i].source));
        arr[i] = e;
    }
    out.Set("events", arr);
    return out;
    NAPI_CATCH
}

Napi::Value GetAuditLog(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata || bdata->auditLogPath.empty()) {
        return Napi::Array::New(env, 0);
    }
    int maxLines = (info.Length() >= 1 && info[0].IsNumber()) ? info[0].As<Napi::Number>().Int32Value() : 200;

    std::ifstream in(bdata->auditLogPath);
    if (!in.is_open()) return Napi::Array::New(env, 0);

    std::vector<std::string> tail;
    tail.reserve(static_cast<size_t>(std::max(1, maxLines)));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        tail.push_back(line);
        if (tail.size() > static_cast<size_t>(std::max(1, maxLines))) tail.erase(tail.begin());
    }

    Napi::Array arr = Napi::Array::New(env, tail.size());
    for (size_t i = 0; i < tail.size(); ++i) {
        arr[i] = Napi::String::New(env, tail[i]);
    }
    return arr;
    NAPI_CATCH
}

Napi::Value StopScan(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();

    if (bdata && bdata->scanContext) {
        bdata->scanContext->coordinator.requestStop();
    }
    return env.Undefined();
    NAPI_CATCH
}

Napi::Value StartScan(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString()) {
        Napi::TypeError::New(env, "Expected drivePath, scanType, [options], callback").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return env.Undefined();

    if (bdata->scanContext) {
        const uint64_t oldGen = bdata->scanContext->generation;
        bdata->scanContext->coordinator.stopScan();
        teardownScanOnJs(bdata, oldGen);
    }

    if (!bdata->tryBeginHeavyOp()) {
        Napi::Error::New(env, "Another disk operation is already running").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string drivePath = info[0].As<Napi::String>().Utf8Value();
    std::string scanType = info[1].As<Napi::String>().Utf8Value();
    Napi::Function cb;
    byteback::ScanTarget target;
    int64_t resumeScanId = -1;
    bool allowSsdDeepScan = false;

    if (info.Length() >= 4 && info[2].IsObject() && info[3].IsFunction()) {
        Napi::Object opts = info[2].As<Napi::Object>();
        cb = info[3].As<Napi::Function>();
        if (opts.Has("allowSsdDeepScan") && opts.Get("allowSsdDeepScan").IsBoolean()) {
            allowSsdDeepScan = opts.Get("allowSsdDeepScan").As<Napi::Boolean>().Value();
        }
        if (opts.Has("partitionStartSector") && opts.Get("partitionStartSector").IsNumber()) {
            target.partitionStartSector =
                static_cast<int64_t>(opts.Get("partitionStartSector").As<Napi::Number>().Int64Value());
        }
        if (opts.Has("partitionSizeInSectors") && opts.Get("partitionSizeInSectors").IsNumber()) {
            target.partitionSizeSectors =
                static_cast<uint64_t>(opts.Get("partitionSizeInSectors").As<Napi::Number>().Int64Value());
        }
        if (opts.Has("partitionIndex") && opts.Get("partitionIndex").IsNumber()) {
            int pidx = opts.Get("partitionIndex").As<Napi::Number>().Int32Value();
            int driveIndex = 0;
            if (drivePath != "raid") {
                try { driveIndex = std::stoi(drivePath); } catch (...) { driveIndex = -1; }
            }
            if (pidx >= 0 && driveIndex >= 0) {
                byteback::DiskReader& reader = bdata->engine.getDiskReader();
                if (!reader.isOpen() || reader.getDriveIndex() != driveIndex) {
                    reader.openDrive(driveIndex);
                }
                byteback::PartitionScanner scanner(&reader);
                std::vector<byteback::PartitionInfo> parts = scanner.parseMBR();
                std::vector<byteback::PartitionInfo> gpt = scanner.parseGPT();
                if (!gpt.empty()) parts = std::move(gpt);
                if (static_cast<size_t>(pidx) < parts.size()) {
                    target.partitionStartSector = static_cast<int64_t>(parts[pidx].startSector);
                    target.partitionSizeSectors = parts[pidx].sizeInSectors;
                }
            }
        }
        if (opts.Has("resumeScanId") && opts.Get("resumeScanId").IsNumber()) {
            resumeScanId = opts.Get("resumeScanId").As<Napi::Number>().Int64Value();
            auto st = bdata->engine.getMetadataStore().getScanState(resumeScanId);
            if (st.id <= 0 || st.status != 4) {
                bdata->endHeavyOp();
                Napi::Error::New(env, "Scan is not resumable (status must be paused)").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            if (st.scanType == "quick") {
                bdata->endHeavyOp();
                Napi::Error::New(env, "Quick scan cannot resume; start a new scan").ThrowAsJavaScriptException();
                return env.Undefined();
            }
            if ((st.scanType == "deep" || st.scanType == "full_carve") && !st.metadataComplete) {
                bdata->endHeavyOp();
                Napi::Error::New(env,
                                 "Cannot resume during metadata phase; stop and start a new scan")
                    .ThrowAsJavaScriptException();
                return env.Undefined();
            }
            target.resumeAtSector = st.scannedSectors;
            target.metadataComplete = st.metadataComplete;
            target.carveResumeSector = st.carveResumeSector;
            if (target.partitionStartSector < 0 && st.partitionStartSector >= 0) {
                target.partitionStartSector = st.partitionStartSector;
                target.partitionSizeSectors = st.partitionSizeSectors;
            }
        }
    } else if (info[2].IsFunction()) {
        cb = info[2].As<Napi::Function>();
    } else {
        bdata->endHeavyOp();
        Napi::TypeError::New(env, "Expected callback function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    auto context = std::make_shared<ScanContext>();
    context->generation = bdata->nextScanGeneration.fetch_add(1);
    bdata->scanContext = context;

    int driveIndex = 0;
    if (drivePath == "raid") {
        if (!bdata->raid) {
            bdata->endHeavyOp();
            Napi::Error::New(env, "No RAID array assembled").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        driveIndex = -1;
    } else {
        try { driveIndex = std::stoi(drivePath); } catch(...) {}
    }

    if ((scanType == "deep" || scanType == "full_carve") && !allowSsdDeepScan && driveIndex >= 0 && drivePath != "raid") {
        byteback::SmartMonitor smart;
        byteback::SmartStatus st = smart.getSmartStatus(driveIndex);
        if (st.isValid && st.isSsd) {
            bdata->endHeavyOp();
            Napi::Error::New(env,
                             "SSD deep carve is unlikely after TRIM; set allowSsdDeepScan to proceed")
                .ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    if (resumeScanId > 0) {
        context->scanId = resumeScanId;
        if (!bdata->engine.getMetadataStore().setScanRunning(resumeScanId)) {
            bdata->endHeavyOp();
            Napi::Error::New(env, "Failed to mark scan running").ThrowAsJavaScriptException();
            return env.Undefined();
        }
    } else {
        context->scanId = bdata->engine.getMetadataStore().createScan(driveIndex, scanType, 0);
    }
    if (target.partitionStartSector >= 0 && target.partitionSizeSectors > 0) {
        bdata->engine.getMetadataStore().setScanPartition(
            context->scanId, target.partitionStartSector, target.partitionSizeSectors);
    }
    context->dedupIndex.clear();
    if (resumeScanId > 0) {
        constexpr int kHydratePage = 1000;
        for (int off = 0;; off += kHydratePage) {
            auto batch = bdata->engine.getMetadataStore().getFiles(context->scanId, off, kHydratePage);
            if (batch.empty()) break;
            context->dedupIndex.loadFromRecords(batch);
            if (static_cast<int>(batch.size()) < kHydratePage) break;
        }
    }

    forensic::AuditLogger::GetInstance().LogEvent(
        "SCAN_START | drive=" + drivePath + " | type=" + scanType + " | scanId=" + std::to_string(context->scanId) +
        (bdata->raid ? " | raid=1" : "") +
        (target.resumeAtSector > 0 ? " | resumeAt=" + std::to_string(target.resumeAtSector) : "") +
        (target.partitionStartSector >= 0
             ? " | partStart=" + std::to_string(target.partitionStartSector) +
                   " | partSectors=" + std::to_string(target.partitionSizeSectors)
             : ""));

    context->tsfn = Napi::ThreadSafeFunction::New(
        env, cb, "ScanCallback", 4096, 1,
        [](Napi::Env) {}
    );

    auto onFileFound = [context, engine = &bdata->engine](const byteback::FileRecord& fr) {
        // USN journal records are timeline events, not scannable files:
        // route them to the timeline_events table with the reason bitmap
        // decoded to a dominant event type.
        if (fr.source == "usn_journal") {
            byteback::TimelineEvent ev;
            ev.scanId = context->scanId;
            ev.timestamp = fr.createdAt;
            ev.mftRef = fr.parentId > 0 ? static_cast<uint64_t>(fr.parentId) : 0;
            ev.source = "usn_journal";
            ev.fileName = fr.name;
            uint32_t reason = static_cast<uint32_t>(fr.status);
            if (reason & 0x00000002)      ev.eventType = "delete";
            else if (reason & 0x00010000) ev.eventType = "rename_old";
            else if (reason & 0x00020000) ev.eventType = "rename_new";
            else if (reason & 0x00000010) ev.eventType = "truncate";
            else if (reason & 0x00000008) ev.eventType = "extend";
            else if (reason & 0x00000004) ev.eventType = "overwrite";
            else if (reason & 0x00000001) ev.eventType = "create";
            else                          ev.eventType = "touch";
            engine->getMetadataStore().insertTimelineEvent(context->scanId, ev);
            return; // not surfaced as a file result
        }
        if (fr.source == "ntfs_logfile" || fr.source == "ntfs_logfile_restart") {
            return; // timeline/discovery hints only
        }

        byteback::FileRecord out = fr;
        context->dedupIndex.observe(out);
        context->dedupIndex.markDuplicate(out);

        {
            std::lock_guard<std::mutex> lock(context->bufferMutex);
            context->fileBuffer.push_back(out);
            if (context->fileBuffer.size() >= 500) {
                engine->getMetadataStore().insertFilesBatch(context->scanId, context->fileBuffer);
                context->fileBuffer.clear();
            }
        }

        if (context->filesPosted.load() < kMaxLiveFileEvents) {
            auto callback = [out](Napi::Env env, Napi::Function jsCallback) {
                Napi::Object obj = FileRecordToJs(env, out);
                obj.Set("type", Napi::String::New(env, "file"));
                obj.Set("size", Napi::Number::New(env, static_cast<double>(out.sizeBytes)));
                jsCallback.Call({obj});
            };
            if (tsfnPost(context->tsfn, callback)) context->filesPosted.fetch_add(1);
        }
    };

    auto totalSectorsSet = std::make_shared<std::atomic<bool>>(false);
    auto lastProgress = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    auto lastDbProgress = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    auto onProgress = [context, lastProgress, lastDbProgress, engine = &bdata->engine, totalSectorsSet](uint64_t current, uint64_t total) {
        if (total > 0 && !totalSectorsSet->exchange(true)) {
            engine->getMetadataStore().setScanTotalSectors(context->scanId, total);
        }
        auto now = std::chrono::steady_clock::now();
        if (current == total ||
            std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastDbProgress).count() >= 500) {
            engine->getMetadataStore().updateScanProgress(context->scanId, current);
            *lastDbProgress = now;
        }

        if (current != total && std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastProgress).count() < 250) {
            return;
        }
        *lastProgress = now;

        const char* phasePtr = byteback::g_scanPhase.load(std::memory_order_relaxed);
        auto callback = [current, total, phase = std::string(phasePtr ? phasePtr : "metadata"),
                         bad = context->badSectors](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("type", Napi::String::New(env, "progress"));
            obj.Set("current", Napi::Number::New(env, static_cast<double>(current)));
            obj.Set("total", Napi::Number::New(env, static_cast<double>(total)));
            obj.Set("phase", Napi::String::New(env, phase));
            Napi::Array badArr = Napi::Array::New(env, bad.size());
            for (size_t i = 0; i < bad.size(); ++i) {
                badArr[i] = Napi::Number::New(env, static_cast<double>(bad[i]));
            }
            obj.Set("badSectors", badArr);
            jsCallback.Call({obj});
        };
        tsfnPost(context->tsfn, callback);
    };

    auto onFinished = [bdata, context](int status) {
        flushScanToDb(bdata, status);
        const uint64_t gen = context->generation;
        const int64_t scanId = context->scanId;
        auto callback = [bdata, scanId, status, gen](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("type", Napi::String::New(env, "complete"));
            obj.Set("scanId", Napi::Number::New(env, static_cast<double>(scanId)));
            obj.Set("status", Napi::Number::New(env, status));
            jsCallback.Call({obj});
            teardownScanOnJs(bdata, gen);
        };
        if (!tsfnPost(context->tsfn, callback)) {
            std::fprintf(stderr, "[byteback] scan complete event dropped (queue full) scanId=%lld\n",
                         static_cast<long long>(scanId));
        }
    };

    const int64_t checkpointScanId = context->scanId;
    byteback::ScanCheckpointCallback onCheckpoint =
        [engine = &bdata->engine, checkpointScanId](bool metadataComplete, uint64_t carveResume) {
            engine->getMetadataStore().updateScanCheckpoint(checkpointScanId, metadataComplete, carveResume);
        };

    context->coordinator.startScan(drivePath, scanType, onFileFound, onProgress,
                                   &context->badSectors, bdata->raid, onFinished,
                                   &bdata->engine.getDiskReader(), target, onCheckpoint);

    return Napi::Number::New(env, static_cast<double>(context->scanId));
    NAPI_CATCH
}

Napi::Value SearchFiles(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    byteback::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine || info.Length() < 4) return env.Undefined();

    int64_t scanId = info[0].As<Napi::Number>().Int64Value();
    std::string query = info[1].As<Napi::String>().Utf8Value();
    int offset = info[2].As<Napi::Number>().Int32Value();
    int limit = info[3].As<Napi::Number>().Int32Value();
    bool useRegex = info.Length() >= 5 && info[4].IsBoolean() && info[4].As<Napi::Boolean>().Value();
    std::string categoryFilter;
    if (info.Length() >= 6 && info[5].IsString()) {
        categoryFilter = info[5].As<Napi::String>().Utf8Value();
    }
    int statusFilter = -1;
    if (info.Length() >= 7 && info[6].IsNumber()) {
        statusFilter = info[6].As<Napi::Number>().Int32Value();
    }

    auto files = engine->getMetadataStore().searchFiles(scanId, query, offset, limit, useRegex, categoryFilter, statusFilter);
    Napi::Array result = Napi::Array::New(env, files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        result[i] = FileRecordToJs(env, files[i]);
    }
    return result;
    NAPI_CATCH
}

namespace {

bool openScanReader(byteback::DiskReader& reader, BridgeData* bdata, const byteback::ScanState& state,
                    byteback::Engine* engine) {
    if (bdata->raid) {
        reader.setRaidBackend(bdata->raid);
    } else {
        if (state.driveIndex < 0) return false;
        if (!reader.openDrive(state.driveIndex)) return false;
    }
    if (engine) reader.copyXtsFvekFrom(engine->getDiskReader());
    return true;
}

} // namespace

Napi::Value SearchFileContent(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    byteback::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine || info.Length() < 4) return env.Undefined();

    int64_t scanId = info[0].As<Napi::Number>().Int64Value();
    std::string query = info[1].As<Napi::String>().Utf8Value();
    int offset = info[2].As<Napi::Number>().Int32Value();
    int limit = info[3].As<Napi::Number>().Int32Value();

    auto state = engine->getMetadataStore().getScanState(scanId);
    if (state.id <= 0) return Napi::Array::New(env, 0);

    byteback::DiskReader reader;
    if (!openScanReader(reader, bdata, state, engine)) {
        return Napi::Array::New(env, 0);
    }

    auto files = byteback::searchFileContent(engine->getMetadataStore(), reader, scanId, query, offset, limit);
    Napi::Array result = Napi::Array::New(env, files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        result[i] = FileRecordToJs(env, files[i]);
    }
    return result;
    NAPI_CATCH
}

Napi::Value StartContentSearch(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsString() || !info[2].IsFunction()) {
        Napi::TypeError::New(env, "Expected scanId, query, callback").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return Napi::Boolean::New(env, false);

    if (bdata->contentSearchContext) {
        bdata->contentSearchContext->coordinator.stopSearch();
        if (bdata->contentSearchContext->holdsHeavyOp) {
            bdata->endHeavyOp();
            bdata->contentSearchContext->holdsHeavyOp = false;
        }
        bdata->contentSearchContext.reset();
    }

    if (!bdata->tryBeginHeavyOp()) {
        Napi::Error::New(env, "Another disk operation is already running").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    int64_t scanId = info[0].As<Napi::Number>().Int64Value();
    std::string query = info[1].As<Napi::String>().Utf8Value();
    Napi::Function cb = info[2].As<Napi::Function>();

    auto state = bdata->engine.getMetadataStore().getScanState(scanId);
    if (state.id <= 0) {
        bdata->endHeavyOp();
        return Napi::Boolean::New(env, false);
    }

    int driveIndex = state.driveIndex;
    if (driveIndex < 0 && !bdata->raid) {
        bdata->endHeavyOp();
        return Napi::Boolean::New(env, false);
    }

    auto context = std::make_shared<ContentSearchContext>();
    context->holdsHeavyOp = true;
    bdata->contentSearchContext = context;
    context->tsfn = Napi::ThreadSafeFunction::New(env, cb, "ContentSearchCallback", 4096, 1,
                                                  [](Napi::Env) {});

    auto onMatch = [context](const byteback::FileRecord& fr) {
        auto callback = [fr](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = FileRecordToJs(env, fr);
            obj.Set("type", Napi::String::New(env, "match"));
            jsCallback.Call({obj});
        };
        tsfnPost(context->tsfn, callback);
    };

    auto lastProgress = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    auto onProgress = [context, lastProgress](uint64_t current, uint64_t total) {
        auto now = std::chrono::steady_clock::now();
        if (current != total &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastProgress).count() < 100) {
            return;
        }
        *lastProgress = now;

        auto callback = [current, total](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("type", Napi::String::New(env, "progress"));
            obj.Set("current", Napi::Number::New(env, static_cast<double>(current)));
            obj.Set("total", Napi::Number::New(env, static_cast<double>(total)));
            jsCallback.Call({obj});
        };
        tsfnPost(context->tsfn, callback);
    };

    auto onFinished = [context, bdata](int status) {
        auto callback = [context, bdata, status](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("type", Napi::String::New(env, "complete"));
            obj.Set("status", Napi::Number::New(env, status));
            jsCallback.Call({obj});
            context->tsfn.Release();
            if (bdata && bdata->contentSearchContext == context) {
                if (context->holdsHeavyOp) {
                    bdata->endHeavyOp();
                    context->holdsHeavyOp = false;
                }
                bdata->contentSearchContext.reset();
            }
        };
        tsfnPost(context->tsfn, callback);
    };

    context->coordinator.startSearch(bdata->engine.getMetadataStore(), driveIndex, bdata->raid,
                                     scanId, query, {}, onMatch, onProgress, onFinished,
                                     &bdata->engine.getDiskReader());
    return Napi::Boolean::New(env, true);
    NAPI_CATCH
}

Napi::Value StopContentSearch(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (bdata && bdata->contentSearchContext) {
        bdata->contentSearchContext->coordinator.requestStop();
        if (bdata->contentSearchContext->holdsHeavyOp) {
            bdata->endHeavyOp();
            bdata->contentSearchContext->holdsHeavyOp = false;
        }
    }
    return env.Undefined();
    NAPI_CATCH
}

Napi::Value GetScanSummary(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    byteback::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine || info.Length() < 1) return env.Undefined();

    int64_t scanId = info[0].As<Napi::Number>().Int64Value();
    auto summary = engine->getMetadataStore().getScanSummary(scanId);

    Napi::Object obj = Napi::Object::New(env);
    obj.Set("totalFiles", Napi::Number::New(env, static_cast<double>(summary.totalFiles)));
    obj.Set("deletedFiles", Napi::Number::New(env, static_cast<double>(summary.deletedFiles)));
    obj.Set("imageFiles", Napi::Number::New(env, static_cast<double>(summary.imageFiles)));
    obj.Set("documentFiles", Napi::Number::New(env, static_cast<double>(summary.documentFiles)));
    obj.Set("videoFiles", Napi::Number::New(env, static_cast<double>(summary.videoFiles)));
    obj.Set("audioFiles", Napi::Number::New(env, static_cast<double>(summary.audioFiles)));
    obj.Set("archiveFiles", Napi::Number::New(env, static_cast<double>(summary.archiveFiles)));
    obj.Set("carvedFiles", Napi::Number::New(env, static_cast<double>(summary.carvedFiles)));
    obj.Set("timelineEvents", Napi::Number::New(env, static_cast<double>(summary.timelineEvents)));
    obj.Set("usnCreates", Napi::Number::New(env, static_cast<double>(summary.usnCreates)));
    obj.Set("usnDeletes", Napi::Number::New(env, static_cast<double>(summary.usnDeletes)));
    obj.Set("usnRenames", Napi::Number::New(env, static_cast<double>(summary.usnRenames)));
    return obj;
    NAPI_CATCH
}
