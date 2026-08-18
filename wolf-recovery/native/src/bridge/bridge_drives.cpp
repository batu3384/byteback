// bridge_drives.cpp — drive enumeration, partition tables, raw sector reads
// and SMART/health queries. See bridge_common.h for the shared context.
#include "bridge_common.h"

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

    constexpr uint32_t kMaxRead = 1024 * 1024;
    if (size == 0 || size > kMaxRead) {
        Napi::TypeError::New(env, "Read size must be between 1 and 1048576 bytes").ThrowAsJavaScriptException();
        return env.Undefined();
    }

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
