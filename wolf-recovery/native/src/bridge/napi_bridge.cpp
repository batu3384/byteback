#include <napi.h>
#include "wolf_engine.h"
#include "scan_coordinator.h"
#include "wolf_smart.h"
#include <cstdlib>

struct ScanContext {
    wolf::ScanCoordinator coordinator;
    Napi::ThreadSafeFunction tsfn;
};

struct BridgeData {
    wolf::Engine engine;
    ScanContext* scanContext = nullptr;
    ~BridgeData() { if (scanContext) delete scanContext; }
};

Napi::Value GetVersion(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    wolf::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine) return Napi::String::New(env, "unknown");
    return Napi::String::New(env, engine->getVersion());
}

Napi::Value IsAdministrator(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    wolf::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine) return Napi::Boolean::New(env, false);
    return Napi::Boolean::New(env, engine->isAdministrator());
}

Napi::Value ListDrives(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
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
}

Napi::Value ReadSectors(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
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
}

Napi::Value GetSmartStatus(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
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
    }
    return obj;
}

Napi::Value InitDatabase(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    wolf::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine || info.Length() < 1) return env.Undefined();

    std::string dbPath = info[0].As<Napi::String>().Utf8Value();
    bool ok = engine->getMetadataStore().open(dbPath);
    return Napi::Boolean::New(env, ok);
}

Napi::Value GetFileCount(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    wolf::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine || info.Length() < 1) return env.Undefined();

    int64_t scanId = info[0].As<Napi::Number>().Int64Value();
    int64_t count = engine->getMetadataStore().getFileCount(scanId);
    return Napi::Number::New(env, static_cast<double>(count));
}

// ---------------- Scan Coordinator ----------------

Napi::Value StartScan(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() || !info[2].IsFunction()) {
        Napi::TypeError::New(env, "Expected drivePath, scanType, callback").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return env.Undefined();

    if (bdata->scanContext) {
        bdata->scanContext->coordinator.stopScan();
        delete bdata->scanContext;
        bdata->scanContext = nullptr;
    }
    
    std::string drivePath = info[0].As<Napi::String>().Utf8Value();
    std::string scanType = info[1].As<Napi::String>().Utf8Value();
    Napi::Function cb = info[2].As<Napi::Function>();
    
    bdata->scanContext = new ScanContext();
    bdata->scanContext->tsfn = Napi::ThreadSafeFunction::New(
        env, cb, "ScanCallback", 0, 1, 
        [](Napi::Env) {}
    );

    auto onFileFound = [bdata](const wolf::FileRecord& fr) {
        if (!bdata->scanContext) return;
        auto callback = [fr](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("type", Napi::String::New(env, "file"));
            obj.Set("name", Napi::String::New(env, fr.name));
            obj.Set("size", Napi::Number::New(env, static_cast<double>(fr.sizeBytes)));
            jsCallback.Call({obj});
        };
        bdata->scanContext->tsfn.BlockingCall(callback);
    };

    auto onProgress = [bdata](uint64_t current, uint64_t total) {
        if (!bdata->scanContext) return;
        auto callback = [current, total](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("type", Napi::String::New(env, "progress"));
            obj.Set("current", Napi::Number::New(env, static_cast<double>(current)));
            obj.Set("total", Napi::Number::New(env, static_cast<double>(total)));
            jsCallback.Call({obj});
        };
        bdata->scanContext->tsfn.BlockingCall(callback);
    };

    bdata->scanContext->coordinator.startScan(drivePath, scanType, onFileFound, onProgress);
    
    return Napi::Boolean::New(env, true);
}

Napi::Value StopScan(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    
    if (bdata && bdata->scanContext) {
        bdata->scanContext->coordinator.stopScan();
        bdata->scanContext->tsfn.Release();
        delete bdata->scanContext;
        bdata->scanContext = nullptr;
    }
    return env.Undefined();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    env.SetInstanceData<BridgeData>(new BridgeData());

    exports.Set("getVersion", Napi::Function::New(env, GetVersion));
    exports.Set("isAdministrator", Napi::Function::New(env, IsAdministrator));
    exports.Set("listDrives", Napi::Function::New(env, ListDrives));
    exports.Set("readSectors", Napi::Function::New(env, ReadSectors));
    exports.Set("initDatabase", Napi::Function::New(env, InitDatabase));
    exports.Set("getFileCount", Napi::Function::New(env, GetFileCount));
    exports.Set("getSmartStatus", Napi::Function::New(env, GetSmartStatus));
    
    exports.Set("startScan", Napi::Function::New(env, StartScan));
    exports.Set("stopScan", Napi::Function::New(env, StopScan));

    return exports;
}

NODE_API_MODULE(wolf_engine, Init)

