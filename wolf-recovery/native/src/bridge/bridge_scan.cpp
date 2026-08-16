// bridge_scan.cpp — scan lifecycle, result pages, scan state, unified
// timeline and the audit-log reader. See bridge_common.h for the context.
#include "bridge_common.h"

Napi::Value InitDatabase(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    wolf::Engine* engine = bdata ? &bdata->engine : nullptr;
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
    }
    return Napi::Boolean::New(env, ok);
    NAPI_CATCH
}

Napi::Value GetFileCount(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    wolf::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine || info.Length() < 1) return env.Undefined();

    int64_t scanId = info[0].As<Napi::Number>().Int64Value();
    int64_t count = engine->getMetadataStore().getFileCount(scanId);
    return Napi::Number::New(env, static_cast<double>(count));
    NAPI_CATCH
}

Napi::Value GetFilesPage(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    wolf::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine || info.Length() < 3) return env.Undefined();

    int64_t scanId = info[0].As<Napi::Number>().Int64Value();
    int offset = info[1].As<Napi::Number>().Int32Value();
    int limit = info[2].As<Napi::Number>().Int32Value();

    auto files = engine->getMetadataStore().getFiles(scanId, offset, limit);
    Napi::Array result = Napi::Array::New(env, files.size());

    for (size_t i = 0; i < files.size(); ++i) {
        Napi::Object fileObj = Napi::Object::New(env);
        fileObj.Set("id", Napi::Number::New(env, static_cast<double>(files[i].id)));
        fileObj.Set("parentId", Napi::Number::New(env, static_cast<double>(files[i].parentId)));
        fileObj.Set("name", Napi::String::New(env, files[i].name));
        fileObj.Set("extension", Napi::String::New(env, files[i].extension));
        fileObj.Set("path", Napi::String::New(env, files[i].path));
        fileObj.Set("sizeBytes", Napi::Number::New(env, static_cast<double>(files[i].sizeBytes)));
        fileObj.Set("status", Napi::Number::New(env, files[i].status));
        fileObj.Set("compressed", Napi::Boolean::New(env, files[i].compressed));
        fileObj.Set("confidence", Napi::Number::New(env, files[i].confidence));
        fileObj.Set("category", Napi::String::New(env, files[i].category));
        result[i] = fileObj;
    }
    return result;
    NAPI_CATCH
}

Napi::Value GetScanState(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    wolf::Engine* engine = bdata ? &bdata->engine : nullptr;
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
        bdata->scanContext->coordinator.stopScan();
        // Flush remaining buffer
        {
            std::lock_guard<std::mutex> lock(bdata->scanContext->bufferMutex);
            if (!bdata->scanContext->fileBuffer.empty()) {
                bdata->engine.getMetadataStore().insertFilesBatch(bdata->scanContext->scanId, bdata->scanContext->fileBuffer);
                bdata->scanContext->fileBuffer.clear();
            }
        }
        bdata->engine.getMetadataStore().completeScan(bdata->scanContext->scanId, 2); // 2 = Complete/Stopped

        bdata->scanContext->tsfn.Release();
        bdata->scanContext.reset();
    }
    return env.Undefined();
    NAPI_CATCH
}

Napi::Value StartScan(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() || !info[2].IsFunction()) {
        Napi::TypeError::New(env, "Expected drivePath, scanType, callback").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return env.Undefined();

    if (bdata->scanContext) {
        bdata->scanContext->coordinator.stopScan();
        bdata->scanContext.reset();
    }

    std::string drivePath = info[0].As<Napi::String>().Utf8Value();
    std::string scanType = info[1].As<Napi::String>().Utf8Value();
    Napi::Function cb = info[2].As<Napi::Function>();

    auto context = std::make_shared<ScanContext>();
    bdata->scanContext = context;

    int driveIndex = 0;
    try { driveIndex = std::stoi(drivePath); } catch(...) {}
    context->scanId = bdata->engine.getMetadataStore().createScan(driveIndex, scanType, 0);

    forensic::AuditLogger::GetInstance().LogEvent(
        "SCAN_START | drive=" + drivePath + " | type=" + scanType + " | scanId=" + std::to_string(context->scanId));

    context->tsfn = Napi::ThreadSafeFunction::New(
        env, cb, "ScanCallback", 0, 1,
        [](Napi::Env) {}
    );

    auto onFileFound = [context, engine = &bdata->engine](const wolf::FileRecord& fr) {
        // USN journal records are timeline events, not scannable files:
        // route them to the timeline_events table with the reason bitmap
        // decoded to a dominant event type.
        if (fr.source == "usn_journal") {
            wolf::TimelineEvent ev;
            ev.scanId = context->scanId;
            ev.timestamp = fr.createdAt;
            ev.mftRef = 0;
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

        {
            std::lock_guard<std::mutex> lock(context->bufferMutex);
            context->fileBuffer.push_back(fr);
            if (context->fileBuffer.size() >= 500) {
                engine->getMetadataStore().insertFilesBatch(context->scanId, context->fileBuffer);
                context->fileBuffer.clear();
            }
        }

        auto callback = [fr](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("type", Napi::String::New(env, "file"));
            obj.Set("name", Napi::String::New(env, fr.name));
            obj.Set("size", Napi::Number::New(env, static_cast<double>(fr.sizeBytes)));
            jsCallback.Call({obj});
        };
        context->tsfn.BlockingCall(callback);
    };

    auto lastProgress = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    auto onProgress = [context, lastProgress](uint64_t current, uint64_t total) {
        auto now = std::chrono::steady_clock::now();
        if (current != total && std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastProgress).count() < 100) {
            return;
        }
        *lastProgress = now;

        auto callback = [current, total, bad = context->badSectors](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("type", Napi::String::New(env, "progress"));
            obj.Set("current", Napi::Number::New(env, static_cast<double>(current)));
            obj.Set("total", Napi::Number::New(env, static_cast<double>(total)));
            Napi::Array badArr = Napi::Array::New(env, bad.size());
            for (size_t i = 0; i < bad.size(); ++i) {
                badArr[i] = Napi::Number::New(env, static_cast<double>(bad[i]));
            }
            obj.Set("badSectors", badArr);
            jsCallback.Call({obj});
        };
        context->tsfn.BlockingCall(callback);
    };

    context->coordinator.startScan(drivePath, scanType, onFileFound, onProgress, &context->badSectors);

    return Napi::Number::New(env, static_cast<double>(context->scanId));
    NAPI_CATCH
}
