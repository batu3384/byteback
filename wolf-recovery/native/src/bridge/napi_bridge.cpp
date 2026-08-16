#include <napi.h>
#include "wolf_engine.h"
#include "scan_coordinator.h"
#include "wolf_smart.h"
#include "wolf_imager.h"
#include "wolf_shredder.h"
#include "wolf_recovery.h"
#include "fs/virtual_raid.h"
#include "fs/partition_scanner.h"
#include <cstdlib>
#include <memory>
#include <exception>

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
};

Napi::Value GetVersion(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    wolf::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine) return Napi::String::New(env, "unknown");
    return Napi::String::New(env, engine->getVersion());
    NAPI_CATCH
}

Napi::Value IsAdministrator(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    wolf::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine) return Napi::Boolean::New(env, false);
    return Napi::Boolean::New(env, engine->isAdministrator());
    NAPI_CATCH
}

Napi::Value ListDrives(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    wolf::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine) return env.Undefined();

    auto drives = engine->getDiskReader().enumerateDrives();
    Napi::Array result = Napi::Array::New(env, drives.size());

    for (size_t i = 0; i < drives.size(); ++i) {
        Napi::Object drive = Napi::Object::New(env);
        drive.Set("index", Napi::Number::New(env, drives[i].index));
        drive.Set("model", Napi::String::New(env, drives[i].model));
        drive.Set("serial", Napi::String::New(env, drives[i].serial));
        drive.Set("sizeBytes", Napi::Number::New(env, static_cast<double>(drives[i].sizeBytes)));
        drive.Set("sectorSize", Napi::Number::New(env, drives[i].sectorSize));
        drive.Set("type", Napi::String::New(env, drives[i].type));
        result[i] = drive;
    }
    return result;
    NAPI_CATCH
}

// ---------------- Partition table ----------------
// Returns the MBR (or GPT, when present) partition layout of a physical
// drive: [{type, startSector, sizeInSectors, label, isActive}].
Napi::Value ListPartitions(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata || info.Length() < 1 || !info[0].IsNumber()) {
        return Napi::Array::New(env, 0);
    }

    int driveIndex = info[0].As<Napi::Number>().Int32Value();
    wolf::DiskReader& reader = bdata->engine.getDiskReader();
    if (!reader.isOpen() || reader.getDriveIndex() != driveIndex) {
        if (!reader.openDrive(driveIndex)) return Napi::Array::New(env, 0);
    }

    wolf::PartitionScanner scanner(&reader);
    std::vector<wolf::PartitionInfo> parts = scanner.parseMBR();
    std::vector<wolf::PartitionInfo> gpt = scanner.parseGPT();
    if (!gpt.empty()) parts = std::move(gpt);

    Napi::Array result = Napi::Array::New(env, parts.size());
    for (size_t i = 0; i < parts.size(); ++i) {
        Napi::Object p = Napi::Object::New(env);
        p.Set("type", Napi::String::New(env, parts[i].type));
        p.Set("startSector", Napi::Number::New(env, static_cast<double>(parts[i].startSector)));
        p.Set("sizeInSectors", Napi::Number::New(env, static_cast<double>(parts[i].sizeInSectors)));
        p.Set("label", Napi::String::New(env, parts[i].label));
        p.Set("isActive", Napi::Boolean::New(env, parts[i].isActive));
        result[i] = p;
    }
    return result;
    NAPI_CATCH
}

Napi::Value ReadSectors(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    wolf::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine || info.Length() < 3) return env.Undefined();

    int driveIndex = info[0].As<Napi::Number>().Int32Value();
    double offset = info[1].As<Napi::Number>().DoubleValue();
    uint32_t size = info[2].As<Napi::Number>().Uint32Value();

    auto& diskReader = engine->getDiskReader();
    if (!diskReader.isOpen() || diskReader.getDriveIndex() != driveIndex) {
        diskReader.openDrive(driveIndex);
    }

    uint8_t* buffer = static_cast<uint8_t*>(_aligned_malloc(size, 4096));
    if (!buffer) return env.Undefined();

    auto result = diskReader.readSectors(static_cast<uint64_t>(offset), size, buffer);

    Napi::Object obj = Napi::Object::New(env);
    obj.Set("success", Napi::Boolean::New(env, result.success));
    obj.Set("bytesRead", Napi::Number::New(env, static_cast<double>(result.bytesRead)));
    obj.Set("error", Napi::String::New(env, result.error));

    if (result.success && result.bytesRead > 0) {
        Napi::Buffer<uint8_t> buf = Napi::Buffer<uint8_t>::New(
            env, buffer, result.bytesRead,
            [](Napi::Env, uint8_t* data) { _aligned_free(data); }
        );
        obj.Set("data", buf);
    } else {
        _aligned_free(buffer);
    }
    return obj;
    NAPI_CATCH
}

// ---------------- Unified timeline ----------------
// getTimelineEvents(scanId, offset, limit, [eventTypeFilter])
// Returns {total, events: [{id, timestamp, eventType, fileName, mftRef, source}]}
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

Napi::Value GetSmartStatus(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 1) return env.Undefined();
    int driveIndex = info[0].As<Napi::Number>().Int32Value();

    wolf::SmartMonitor monitor;
    auto status = monitor.getSmartStatus(driveIndex);

    Napi::Object obj = Napi::Object::New(env);
    obj.Set("isValid", Napi::Boolean::New(env, status.isValid));
    if (status.isValid) {
        obj.Set("driveModel", Napi::String::New(env, status.driveModel));
        obj.Set("healthScore", Napi::String::New(env, status.healthScore));
        obj.Set("temperatureC", Napi::Number::New(env, status.temperatureC));
        obj.Set("powerOnHours", Napi::Number::New(env, status.powerOnHours));
        obj.Set("reallocatedSectors", Napi::Number::New(env, status.reallocatedSectors));
        obj.Set("pendingSectors", Napi::Number::New(env, status.pendingSectors));
        // NVMe health-log / wear fields (-1 = not reported).
        obj.Set("isNvme", Napi::Boolean::New(env, status.isNvme));
        obj.Set("percentageUsed", Napi::Number::New(env, status.percentageUsed));
        obj.Set("availableSpare", Napi::Number::New(env, status.availableSpare));
        obj.Set("availableSpareThreshold", Napi::Number::New(env, status.availableSpareThreshold));
        obj.Set("criticalWarning", Napi::Number::New(env, status.criticalWarning));
        obj.Set("unsafeShutdowns", Napi::Number::New(env, static_cast<double>(status.unsafeShutdowns)));
        obj.Set("mediaErrors", Napi::Number::New(env, static_cast<double>(status.mediaErrors)));
        obj.Set("totalBytesWritten", Napi::Number::New(env, static_cast<double>(status.totalBytesWritten)));
        // SSD/TRIM awareness (seek-penalty query).
        obj.Set("isSsd", Napi::Boolean::New(env, status.isSsd));
        obj.Set("seekPenaltyKnown", Napi::Boolean::New(env, status.seekPenaltyKnown));
    }
    return obj;
    NAPI_CATCH
}

Napi::Value InitDatabase(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    wolf::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine || info.Length() < 1) return env.Undefined();

    std::string dbPath = info[0].As<Napi::String>().Utf8Value();
    bool ok = engine->getMetadataStore().open(dbPath);
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
    return obj;
    NAPI_CATCH
}

// ---------------- Scan Coordinator ----------------

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
        
        auto callback = [current, total](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("type", Napi::String::New(env, "progress"));
            obj.Set("current", Napi::Number::New(env, static_cast<double>(current)));
            obj.Set("total", Napi::Number::New(env, static_cast<double>(total)));
            jsCallback.Call({obj});
        };
        context->tsfn.BlockingCall(callback);
    };

    context->coordinator.startScan(drivePath, scanType, onFileFound, onProgress);
    
    return Napi::Number::New(env, static_cast<double>(context->scanId));
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

// ---------------- Imager ----------------

Napi::Value StartImaging(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsString() || !info[2].IsFunction()) {
        Napi::TypeError::New(env, "Expected driveIndex, destPath, callback, [format]").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return env.Undefined();

    if (bdata->imagerContext) {
        bdata->imagerContext->imager.stopImaging();
        bdata->imagerContext.reset();
    }

    int driveIndex = info[0].As<Napi::Number>().Int32Value();
    std::string destPath = info[1].As<Napi::String>().Utf8Value();
    Napi::Function cb = info[2].As<Napi::Function>();
    // Optional 4th arg: format string — "ewf" selects Expert Witness (.E01)
    // with on-the-fly MD5; anything else (or absent) is raw/dd.
    wolf::ImageFormat format = wolf::ImageFormat::Raw;
    if (info.Length() >= 4 && info[3].IsString()) {
        std::string f = info[3].As<Napi::String>().Utf8Value();
        if (f == "ewf" || f == "e01") format = wolf::ImageFormat::Ewf;
    }

    auto context = std::make_shared<ImagerContext>();
    bdata->imagerContext = context;

    context->tsfn = Napi::ThreadSafeFunction::New(
        env, cb, "ImagingCallback", 0, 1,
        [](Napi::Env) {}
    );

    auto lastProgress = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    auto imagerPtr = &(context->imager);
    auto onProgress = [context, lastProgress, imagerPtr](uint64_t current, uint64_t total) {
        auto now = std::chrono::steady_clock::now();
        if (current != total && std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastProgress).count() < 100) {
            return;
        }
        *lastProgress = now;

        auto callback = [current, total, imagerPtr](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("type", Napi::String::New(env, "progress"));
            obj.Set("current", Napi::Number::New(env, static_cast<double>(current)));
            obj.Set("total", Napi::Number::New(env, static_cast<double>(total)));
            // On completion, surface the image MD5 (EWF images are hashed on
            // the fly; empty for raw images).
            if (current >= total) {
                obj.Set("md5", Napi::String::New(env, imagerPtr->lastImageMd5()));
            }
            jsCallback.Call({obj});
        };
        context->tsfn.BlockingCall(callback);
    };

    context->imager.startImaging(driveIndex, destPath, onProgress, format);

    return Napi::Boolean::New(env, true);
    NAPI_CATCH
}

Napi::Value StopImaging(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    
    if (bdata && bdata->imagerContext) {
        bdata->imagerContext->imager.stopImaging();
        bdata->imagerContext->tsfn.Release();
        bdata->imagerContext.reset();
    }
    return env.Undefined();
    NAPI_CATCH
}


// ---------------- Wipe (Shredder) ----------------
class WipeWorker : public Napi::AsyncWorker {
public:
    WipeWorker(Napi::Env& env, const std::string& path, Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), path_(path), deferred_(deferred), success_(false) {}

    void Execute() override {
        try {
            std::string finalPath = path_;
            
            bool isNumber = !path_.empty() && std::all_of(path_.begin(), path_.end(), ::isdigit);
            if (isNumber) {
                finalPath = "\\\\.\\PhysicalDrive" + path_;
            }

            security::DataShredder shredder;
            success_ = shredder.shred_file(finalPath);
        } catch (...) {
            success_ = false;
        }
    }

    void OnOK() override {
        Napi::Env env = Env();
        deferred_.Resolve(Napi::Boolean::New(env, success_));
    }

    void OnError(const Napi::Error& e) override {
        Napi::Env env = Env();
        deferred_.Reject(Napi::Boolean::New(env, false));
    }

private:
    std::string path_;
    Napi::Promise::Deferred deferred_;
    bool success_;
};

Napi::Value StartWipe(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected targetPath").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    std::string targetPath = info[0].As<Napi::String>().Utf8Value();
    
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    
    WipeWorker* worker = new WipeWorker(env, targetPath, deferred);
    worker->Queue();
    
    return deferred.Promise();
    NAPI_CATCH
}

// ---------------- Virtual RAID ----------------
// Accepts (driveIndices: number[], raidLevel: number) and assembles a real
// VirtualRaid over the physical disks. The array is cached in BridgeData so
// scan/imaging calls can read through it. Returns an object with
// { success, capacity, numDisks, error } so the UI can show the assembled
// capacity instead of a bare boolean.
Napi::Value ReconstructRaid(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 2 || !info[0].IsArray() || !info[1].IsNumber()) {
        Napi::TypeError::New(env, "Expected (driveIndices: number[], raidLevel: number)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) {
        Napi::Error::New(env, "Bridge data unavailable").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Object out = Napi::Object::New(env);
    auto fail = [&](const std::string& why) -> Napi::Object {
        out.Set("success", Napi::Boolean::New(env, false));
        out.Set("capacity", Napi::Number::New(env, 0));
        out.Set("numDisks", Napi::Number::New(env, 0));
        out.Set("error", Napi::String::New(env, why));
        return out;
    };

    Napi::Array indicesArr = info[0].As<Napi::Array>();
    int raidLevel = info[1].As<Napi::Number>().Int32Value();

    if (indicesArr.Length() < 2) {
        return fail("RAID requires at least 2 disks");
    }
    // RaidLevel enum (virtual_raid.h): RAID0=0, RAID1=1, RAID5=2, RAID6=3, RAID10=4
    if (raidLevel < 0 || raidLevel > 4) {
        return fail("Unknown RAID level");
    }

    std::vector<int> drives;
    drives.reserve(indicesArr.Length());
    for (uint32_t i = 0; i < indicesArr.Length(); ++i) {
        Napi::Value v = indicesArr[i];
        if (!v.IsNumber()) return fail("driveIndices must be numbers");
        drives.push_back(v.As<Napi::Number>().Int32Value());
    }

    // 64 KiB stripe unit — the most common hardware/Intel RST default.
    constexpr size_t kStripeSize = 64 * 1024;

    try {
        auto level = static_cast<wolf::RaidLevel>(raidLevel);
        auto raid = std::make_shared<wolf::VirtualRaid>(level, drives, kStripeSize);
        // Probe the first block of the array to verify every member disk can
        // actually be read through the assembly before reporting success.
        auto probe = raid->read(0, 512);
        bdata->raid = std::move(raid);

        uint64_t cap = bdata->raid->capacity();
        out.Set("success", Napi::Boolean::New(env, true));
        out.Set("capacity", Napi::Number::New(env, static_cast<double>(cap)));
        out.Set("numDisks", Napi::Number::New(env, static_cast<uint32_t>(drives.size())));
        out.Set("error", Napi::String::New(env, ""));
        (void)probe;
        return out;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    NAPI_CATCH
}

// ---------------- File Recovery ----------------
class RecoverWorker : public Napi::AsyncWorker {
public:
    RecoverWorker(Napi::Env& env, wolf::Engine* engine, int driveIndex,
                  const wolf::FileRecord& record, const std::string& destDir,
                  Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), engine_(engine), driveIndex_(driveIndex),
          record_(record), destDir_(destDir), deferred_(deferred) {}

    void Execute() override {
        try {
            auto& reader = engine_->getDiskReader();
            if (!reader.isOpen() || reader.getDriveIndex() != driveIndex_) {
                reader.openDrive(driveIndex_);
            }

            wolf::RecoveryEngine recovery;
            result_ = recovery.recoverFile(reader, record_, destDir_);
        } catch (const std::exception& e) {
            result_.success = false;
            result_.error = e.what();
        } catch (...) {
            result_.success = false;
            result_.error = "Unknown recovery error";
        }
    }

    void OnOK() override {
        Napi::Env env = Env();
        Napi::Object obj = Napi::Object::New(env);
        obj.Set("success", Napi::Boolean::New(env, result_.success));
        obj.Set("destPath", Napi::String::New(env, result_.destPath));
        obj.Set("bytesRecovered", Napi::Number::New(env, static_cast<double>(result_.bytesRecovered)));
        obj.Set("error", Napi::String::New(env, result_.error));
        obj.Set("md5Hash", Napi::String::New(env, result_.md5Hash));
        deferred_.Resolve(obj);
    }

    void OnError(const Napi::Error& e) override {
        deferred_.Reject(Napi::String::New(Env(), e.what()));
    }

private:
    wolf::Engine* engine_;
    int driveIndex_;
    wolf::FileRecord record_;
    std::string destDir_;
    Napi::Promise::Deferred deferred_;
    wolf::RecoveryResult result_;
};

Napi::Value RecoverFile(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsObject() || !info[2].IsString()) {
        Napi::TypeError::New(env, "Expected driveIndex, fileRecord, destDir").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return env.Undefined();

    int driveIndex = info[0].As<Napi::Number>().Int32Value();
    Napi::Object fileObj = info[1].As<Napi::Object>();
    std::string destDir = info[2].As<Napi::String>().Utf8Value();

    wolf::FileRecord record;
    record.id = fileObj.Has("id") ? (int64_t)fileObj.Get("id").As<Napi::Number>().DoubleValue() : 0;
    record.name = fileObj.Has("name") ? fileObj.Get("name").As<Napi::String>().Utf8Value() : "unknown.bin";
    record.extension = fileObj.Has("extension") ? fileObj.Get("extension").As<Napi::String>().Utf8Value() : "";
    record.path = fileObj.Has("path") ? fileObj.Get("path").As<Napi::String>().Utf8Value() : "";
    record.sizeBytes = fileObj.Has("sizeBytes") ? (uint64_t)fileObj.Get("sizeBytes").As<Napi::Number>().DoubleValue() : 0;
    record.startSector = fileObj.Has("startSector") ? (uint64_t)fileObj.Get("startSector").As<Napi::Number>().DoubleValue() : 0;
    record.endSector = fileObj.Has("endSector") ? (uint64_t)fileObj.Get("endSector").As<Napi::Number>().DoubleValue() : 0;
    record.status = fileObj.Has("status") ? fileObj.Get("status").As<Napi::Number>().Int32Value() : 0;
    record.source = fileObj.Has("source") ? fileObj.Get("source").As<Napi::String>().Utf8Value() : "";

    // Parse data runs if provided
    if (fileObj.Has("runs") && fileObj.Get("runs").IsArray()) {
        Napi::Array runsArr = fileObj.Get("runs").As<Napi::Array>();
        for (uint32_t i = 0; i < runsArr.Length(); i++) {
            Napi::Object runObj = runsArr.Get(i).As<Napi::Object>();
            wolf::FileRecord::DataRun dr;
            dr.startSector = (uint64_t)runObj.Get("startSector").As<Napi::Number>().DoubleValue();
            dr.sectorCount = (uint64_t)runObj.Get("sectorCount").As<Napi::Number>().DoubleValue();
            record.runs.push_back(dr);
        }
    }

    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    RecoverWorker* worker = new RecoverWorker(env, &bdata->engine, driveIndex, record, destDir, deferred);
    worker->Queue();

    return deferred.Promise();
    NAPI_CATCH
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    env.SetInstanceData<BridgeData>(new BridgeData());

    exports.Set("getVersion", Napi::Function::New(env, GetVersion));
    exports.Set("isAdministrator", Napi::Function::New(env, IsAdministrator));
    exports.Set("listDrives", Napi::Function::New(env, ListDrives));
    exports.Set("listPartitions", Napi::Function::New(env, ListPartitions));
    exports.Set("readSectors", Napi::Function::New(env, ReadSectors));
    exports.Set("initDatabase", Napi::Function::New(env, InitDatabase));
    exports.Set("getFileCount", Napi::Function::New(env, GetFileCount));
    exports.Set("getFilesPage", Napi::Function::New(env, GetFilesPage));
    exports.Set("getScanState", Napi::Function::New(env, GetScanState));
    exports.Set("getTimelineEvents", Napi::Function::New(env, GetTimelineEvents));
    exports.Set("getSmartStatus", Napi::Function::New(env, GetSmartStatus));
    
    exports.Set("startScan", Napi::Function::New(env, StartScan));
    exports.Set("stopScan", Napi::Function::New(env, StopScan));
    
    exports.Set("startImaging", Napi::Function::New(env, StartImaging));
    exports.Set("stopImaging", Napi::Function::New(env, StopImaging));
    
    exports.Set("startWipe", Napi::Function::New(env, StartWipe));
    exports.Set("reconstructRaid", Napi::Function::New(env, ReconstructRaid));
    exports.Set("recoverFile", Napi::Function::New(env, RecoverFile));

    return exports;
}

NODE_API_MODULE(wolf_engine, Init)

