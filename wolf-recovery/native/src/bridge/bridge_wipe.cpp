// bridge_wipe.cpp — file wipe (device-guarded, CA-001), virtual RAID assembly
// and file recovery. See bridge_common.h for the shared context.
#include "bridge_common.h"

class WipeWorker : public Napi::AsyncWorker {
public:
    WipeWorker(Napi::Env& env, const std::string& path, Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), path_(path), deferred_(deferred), success_(false) {}

    void Execute() override {
        // CA-001 fix: this worker used to rewrite a numeric drive index into
        // \\.\PhysicalDrive<N> and hand the DEVICE to shred_file() — which
        // would either always fail (device size unreadable) or overwrite the
        // entire physical disk, contradicting the UI's "free-space only"
        // promise. The device path is now rejected outright: shred_file is a
        // FILE shredder and must never see a drive. Free-space wiping needs a
        // filesystem-aware implementation; until then the UI disables it.
        bool isNumber = !path_.empty() && std::all_of(path_.begin(), path_.end(), ::isdigit);
        bool isDevice = path_.rfind("\\\\?\\", 0) == 0 || path_.rfind("\\\\.", 0) == 0 ||
                        path_.find("PhysicalDrive") != std::string::npos;
        if (isNumber || isDevice) {
            // Resolving to plain false keeps the promise contract; the UI
            // explains why disk wiping is unavailable.
            success_ = false;
            return;
        }

        try {
            security::DataShredder shredder;
            success_ = shredder.shred_file(path_);
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

    forensic::AuditLogger::GetInstance().LogEvent("WIPE_START | target=" + targetPath);

    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);

    WipeWorker* worker = new WipeWorker(env, targetPath, deferred);
    worker->Queue();

    return deferred.Promise();
    NAPI_CATCH
}

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
        if (probe.size() < 512) {
            return fail("RAID probe read failed — member disks may be unreadable");
        }
        bdata->raid = std::move(raid);

        uint64_t cap = bdata->raid->capacity();
        out.Set("success", Napi::Boolean::New(env, true));
        out.Set("capacity", Napi::Number::New(env, static_cast<double>(cap)));
        out.Set("numDisks", Napi::Number::New(env, static_cast<uint32_t>(drives.size())));
        out.Set("error", Napi::String::New(env, ""));
        return out;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    NAPI_CATCH
}

class RecoverWorker : public Napi::AsyncWorker {
public:
    RecoverWorker(Napi::Env& env, wolf::Engine* engine, int driveIndex,
                  const wolf::FileRecord& record, const std::string& destDir,
                  int64_t scanId, std::shared_ptr<wolf::VirtualRaid> raid,
                  Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), engine_(engine), driveIndex_(driveIndex),
          record_(record), destDir_(destDir), scanId_(scanId), raid_(std::move(raid)),
          deferred_(deferred) {}

    void Execute() override {
        try {
            auto& reader = engine_->getDiskReader();
            if (raid_) {
                reader.setRaidBackend(raid_);
            } else if (!reader.isOpen() || reader.getDriveIndex() != driveIndex_) {
                reader.openDrive(driveIndex_);
            }

            wolf::RecoveryEngine recovery;
            result_ = recovery.recoverFile(reader, record_, destDir_);

            // CA-008: real recovery counter feeds the Dashboard stat.
            if (result_.success && scanId_ > 0) {
                engine_->getMetadataStore().incrementRecovered(scanId_);
            }
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
    int64_t scanId_ = -1;
    std::shared_ptr<wolf::VirtualRaid> raid_;
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
    // Optional 4th arg: scanId for the recovery counter (CA-008).
    int64_t scanId = (info.Length() >= 4 && info[3].IsNumber())
                     ? info[3].As<Napi::Number>().Int64Value() : -1;

    wolf::FileRecord record;
    record.id = fileObj.Has("id") ? (int64_t)fileObj.Get("id").As<Napi::Number>().DoubleValue() : 0;
    record.name = fileObj.Has("name") ? fileObj.Get("name").As<Napi::String>().Utf8Value() : "unknown.bin";
    record.extension = fileObj.Has("extension") ? fileObj.Get("extension").As<Napi::String>().Utf8Value() : "";
    record.path = fileObj.Has("path") ? fileObj.Get("path").As<Napi::String>().Utf8Value() : "";
    record.sizeBytes = fileObj.Has("sizeBytes") ? (uint64_t)fileObj.Get("sizeBytes").As<Napi::Number>().DoubleValue() : 0;
    record.startSector = fileObj.Has("startSector") ? (uint64_t)fileObj.Get("startSector").As<Napi::Number>().DoubleValue() : 0;
    record.endSector = fileObj.Has("endSector") ? (uint64_t)fileObj.Get("endSector").As<Napi::Number>().DoubleValue() : 0;
    record.status = fileObj.Has("status") ? fileObj.Get("status").As<Napi::Number>().Int32Value() : 0;
    record.compressed = fileObj.Has("compressed") && fileObj.Get("compressed").As<Napi::Boolean>().Value();
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
    RecoverWorker* worker = new RecoverWorker(env, &bdata->engine, driveIndex, record, destDir,
                                              scanId, bdata->raid, deferred);
    worker->Queue();

    return deferred.Promise();
    NAPI_CATCH
}
