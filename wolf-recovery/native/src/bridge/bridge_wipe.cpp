// bridge_wipe.cpp — file / free-space wipe (device-guarded, CA-001), virtual RAID assembly
// and file recovery. See bridge_common.h for the shared context.
#include "bridge_common.h"
#include "fs/vss_scanner.h"
#include "fs/bitlocker_unlock.h"
#include "recovery/preview_reader.h"
#include <filesystem>

namespace {

Napi::Object recoveryResultToJs(Napi::Env env, const wolf::RecoveryResult& r) {
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("success", Napi::Boolean::New(env, r.success));
    obj.Set("destPath", Napi::String::New(env, r.destPath));
    obj.Set("bytesRecovered", Napi::Number::New(env, static_cast<double>(r.bytesRecovered)));
    obj.Set("error", Napi::String::New(env, r.error));
    obj.Set("md5Hash", Napi::String::New(env, r.md5Hash));
    obj.Set("zeroFilled", Napi::Boolean::New(env, r.zeroFilled));
    obj.Set("validationScore", Napi::Number::New(env, r.validationScore));
    obj.Set("validationError", Napi::String::New(env, r.validationError));
    return obj;
}

} // namespace

class WipeWorker : public Napi::AsyncWorker {
public:
    WipeWorker(Napi::Env& env, BridgeData* bdata, const std::string& path, Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), bdata_(bdata), path_(path), deferred_(deferred), success_(false) {}

    void Execute() override {
        // Device paths never reach shred_file. Directories use filler + DoD
        // free-space wipe; files use three-pass shred. PhysicalDrive is refused.
        bool isNumber = !path_.empty() && std::all_of(path_.begin(), path_.end(), ::isdigit);
        bool isDevice = path_.rfind("\\\\?\\", 0) == 0 || path_.rfind("\\\\.", 0) == 0 ||
                        path_.find("PhysicalDrive") != std::string::npos;
        if (isNumber || isDevice) {
            success_ = false;
            return;
        }

        try {
            security::DataShredder shredder;
            std::error_code ec;
            if (std::filesystem::is_directory(path_, ec)) {
                success_ = shredder.shred_free_space(path_, 0);
            } else {
                success_ = shredder.shred_file(path_);
            }
        } catch (...) {
            success_ = false;
        }
    }

    void OnOK() override {
        if (bdata_) bdata_->endHeavyOp();
        Napi::Env env = Env();
        deferred_.Resolve(Napi::Boolean::New(env, success_));
    }

    void OnError(const Napi::Error& e) override {
        if (bdata_) bdata_->endHeavyOp();
        Napi::Env env = Env();
        deferred_.Reject(Napi::Boolean::New(env, false));
        (void)e;
    }

private:
    BridgeData* bdata_;
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

    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return Napi::Boolean::New(env, false);
    if (!bdata->tryBeginHeavyOp()) return Napi::Boolean::New(env, false);

    forensic::AuditLogger::GetInstance().LogEvent("WIPE_START | target=" + targetPath);

    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);

    WipeWorker* worker = new WipeWorker(env, bdata, targetPath, deferred);
    worker->Queue();

    return deferred.Promise();
    NAPI_CATCH
}

static bool parseFvekHex(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() != 64 && hex.size() != 128) return false;
    out.resize(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < out.size(); ++i) {
        int hi = nibble(hex[i * 2]);
        int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

Napi::Value SetBitLockerFvek(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata || info.Length() < 1 || !info[0].IsString()) {
        return Napi::Boolean::New(env, false);
    }
    std::string hex = info[0].As<Napi::String>().Utf8Value();
    wolf::DiskReader& reader = bdata->engine.getDiskReader();
    if (hex.empty()) {
        reader.clearXtsFvek();
        return Napi::Boolean::New(env, true);
    }
    std::vector<uint8_t> key;
    if (!parseFvekHex(hex, key)) return Napi::Boolean::New(env, false);
    bool ok = reader.setXtsFvek(key.data(), key.size());
    if (ok) forensic::AuditLogger::GetInstance().LogEvent("BITLOCKER_FVEK_SET");
    return Napi::Boolean::New(env, ok);
    NAPI_CATCH
}

Napi::Value SetBitLockerRecoveryPassword(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata || info.Length() < 2 || !info[0].IsNumber() || !info[1].IsString()) {
        Napi::TypeError::New(env, "Expected driveIndex, recoveryPassword").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    int driveIndex = info[0].As<Napi::Number>().Int32Value();
    std::string password = info[1].As<Napi::String>().Utf8Value();
    wolf::DiskReader& reader = bdata->engine.getDiskReader();
    if (!reader.isOpen() || reader.getDriveIndex() != driveIndex) {
        if (!reader.openDrive(driveIndex)) {
            return Napi::String::New(env, "drive open failed");
        }
    }
    auto unlocked = wolf::unlockBitLockerWithRecoveryPassword(reader, password, 0);
    if (!unlocked.success) {
        return Napi::String::New(env, unlocked.error);
    }
    if (!reader.setXtsFvek(unlocked.fvek.data(), unlocked.fvek.size())) {
        return Napi::String::New(env, "FVEK apply failed");
    }
    forensic::AuditLogger::GetInstance().LogEvent("BITLOCKER_RECOVERY_UNLOCK");
    return Napi::String::New(env, "");
    NAPI_CATCH
}

Napi::Value SetBitLockerPassword(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata || info.Length() < 2 || !info[0].IsNumber() || !info[1].IsString()) {
        Napi::TypeError::New(env, "Expected driveIndex, password").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    int driveIndex = info[0].As<Napi::Number>().Int32Value();
    std::string password = info[1].As<Napi::String>().Utf8Value();
    wolf::DiskReader& reader = bdata->engine.getDiskReader();
    if (!reader.isOpen() || reader.getDriveIndex() != driveIndex) {
        if (!reader.openDrive(driveIndex)) {
            return Napi::String::New(env, "drive open failed");
        }
    }
    auto unlocked = wolf::unlockBitLockerWithPassword(reader, password, 0);
    if (!unlocked.success) {
        return Napi::String::New(env, unlocked.error);
    }
    if (!reader.setXtsFvek(unlocked.fvek.data(), unlocked.fvek.size())) {
        return Napi::String::New(env, "FVEK apply failed");
    }
    forensic::AuditLogger::GetInstance().LogEvent("BITLOCKER_PASSWORD_UNLOCK");
    return Napi::String::New(env, "");
    NAPI_CATCH
}

class PhysicalWipeWorker : public Napi::AsyncWorker {
public:
    PhysicalWipeWorker(Napi::Env& env, BridgeData* bdata, int index, std::string typed, std::string actual,
                       uint64_t sizeBytes, Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), bdata_(bdata), index_(index), typed_(std::move(typed)), actual_(std::move(actual)),
          sizeBytes_(sizeBytes), deferred_(deferred), success_(false) {}

    void Execute() override {
        security::DataShredder shredder;
        success_ = shredder.shred_physical_drive(index_, typed_, actual_, sizeBytes_);
    }
    void OnOK() override {
        if (bdata_) bdata_->endHeavyOp();
        deferred_.Resolve(Napi::Boolean::New(Env(), success_));
    }
    void OnError(const Napi::Error& e) override {
        if (bdata_) bdata_->endHeavyOp();
        deferred_.Reject(Napi::Boolean::New(Env(), false));
        (void)e;
    }
private:
    BridgeData* bdata_;
    int index_;
    std::string typed_;
    std::string actual_;
    uint64_t sizeBytes_;
    Napi::Promise::Deferred deferred_;
    bool success_;
};

Napi::Value StartPhysicalWipe(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsString()) {
        Napi::TypeError::New(env, "Expected driveIndex, typedSerial").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return Napi::Boolean::New(env, false);
    if (!bdata->tryBeginHeavyOp()) return Napi::Boolean::New(env, false);
    int index = info[0].As<Napi::Number>().Int32Value();
    std::string typed = info[1].As<Napi::String>().Utf8Value();
    auto drives = bdata->engine.getDiskReader().enumerateDrives();
    std::string actual;
    uint64_t sizeBytes = 0;
    for (const auto& d : drives) {
        if (d.index == index) {
            actual = d.serial;
            sizeBytes = d.sizeBytes;
            break;
        }
    }
    if (actual.empty() || sizeBytes < 512) {
        bdata->endHeavyOp();
        return Napi::Boolean::New(env, false);
    }
    forensic::AuditLogger::GetInstance().LogEvent(
        "PHYSICAL_WIPE_START | drive=" + std::to_string(index));
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    (new PhysicalWipeWorker(env, bdata, index, typed, actual, sizeBytes, deferred))->Queue();
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

Napi::Value FailRaidDisk(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 1 || !info[0].IsNumber()) {
        Napi::TypeError::New(env, "Expected diskIndex").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata || !bdata->raid) return Napi::Boolean::New(env, false);
    const int idx = info[0].As<Napi::Number>().Int32Value();
    if (idx < 0) return Napi::Boolean::New(env, false);
    try {
        bdata->raid->fail_disk(static_cast<size_t>(idx));
        return Napi::Boolean::New(env, true);
    } catch (...) {
        return Napi::Boolean::New(env, false);
    }
    NAPI_CATCH
}

class RecoverWorker : public Napi::AsyncWorker {
public:
    RecoverWorker(Napi::Env& env, wolf::Engine* engine, int driveIndex,
                  int64_t fileId, const std::string& destDir,
                  int64_t scanId, std::shared_ptr<wolf::VirtualRaid> raid,
                  Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), engine_(engine), driveIndex_(driveIndex),
          fileId_(fileId), destDir_(destDir), scanId_(scanId), raid_(std::move(raid)),
          deferred_(deferred) {}

    void Execute() override {
        try {
            wolf::FileRecord rec;
            std::string err;
            if (!wolf::loadRecoverRecord(engine_->getMetadataStore(), scanId_, fileId_, rec, err)) {
                result_.success = false;
                result_.error = err;
                return;
            }
            wolf::DiskReader reader;
            if (!wolf::bindReaderForRecord(reader, rec, driveIndex_, raid_, err)) {
                result_.success = false;
                result_.error = err;
                return;
            }
            wolf::applyBoundFvek(reader, engine_->getDiskReader(), rec);
            wolf::RecoveryEngine recovery;
            result_ = recovery.recoverFile(reader, rec, destDir_);
            if (wolf::countsAsRecovered(result_) && scanId_ > 0) {
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
        deferred_.Resolve(recoveryResultToJs(env, result_));
    }

    void OnError(const Napi::Error& e) override {
        deferred_.Reject(Napi::String::New(Env(), e.what()));
    }

private:
    wolf::Engine* engine_;
    int driveIndex_;
    int64_t fileId_ = 0;
    std::string destDir_;
    int64_t scanId_ = -1;
    std::shared_ptr<wolf::VirtualRaid> raid_;
    Napi::Promise::Deferred deferred_;
    wolf::RecoveryResult result_;
};

Napi::Value RecoverFile(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 4 || !info[0].IsNumber() || !info[1].IsNumber() ||
        !info[2].IsString() || !info[3].IsNumber()) {
        Napi::TypeError::New(env, "Expected driveIndex, fileId, destDir, scanId").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return env.Undefined();

    int driveIndex = info[0].As<Napi::Number>().Int32Value();
    int64_t fileId = info[1].As<Napi::Number>().Int64Value();
    std::string destDir = info[2].As<Napi::String>().Utf8Value();
    int64_t scanId = info[3].As<Napi::Number>().Int64Value();

    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    RecoverWorker* worker = new RecoverWorker(env, &bdata->engine, driveIndex, fileId, destDir,
                                              scanId, bdata->raid, deferred);
    worker->Queue();
    return deferred.Promise();
    NAPI_CATCH
}

Napi::Value GetRaidState(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    Napi::Object out = Napi::Object::New(env);
    if (!bdata || !bdata->raid) {
        out.Set("active", Napi::Boolean::New(env, false));
        out.Set("capacity", Napi::Number::New(env, 0));
        out.Set("numDisks", Napi::Number::New(env, 0));
        out.Set("level", Napi::Number::New(env, -1));
        out.Set("failedDisks", Napi::Array::New(env, 0));
        return out;
    }
    out.Set("active", Napi::Boolean::New(env, true));
    out.Set("capacity", Napi::Number::New(env, static_cast<double>(bdata->raid->capacity())));
    out.Set("numDisks", Napi::Number::New(env, static_cast<double>(bdata->raid->num_disks())));
    out.Set("level", Napi::Number::New(env, static_cast<int>(bdata->raid->level())));
    std::vector<uint32_t> failed;
    for (size_t i = 0; i < bdata->raid->num_disks(); ++i) {
        if (!bdata->raid->is_disk_active(i)) failed.push_back(static_cast<uint32_t>(i));
    }
    Napi::Array arr = Napi::Array::New(env, failed.size());
    for (size_t i = 0; i < failed.size(); ++i) arr[i] = Napi::Number::New(env, failed[i]);
    out.Set("failedDisks", arr);
    return out;
    NAPI_CATCH
}

class BatchRecoverWorker : public Napi::AsyncWorker {
public:
    BatchRecoverWorker(Napi::Env& env, wolf::Engine* engine, int driveIndex,
                       std::vector<int64_t> fileIds, const std::string& destDir,
                       int64_t scanId, std::shared_ptr<wolf::VirtualRaid> raid,
                       Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), engine_(engine), driveIndex_(driveIndex),
          fileIds_(std::move(fileIds)), destDir_(destDir), scanId_(scanId),
          raid_(std::move(raid)), deferred_(deferred) {}

    void Execute() override {
        try {
            wolf::RecoveryEngine recovery;
            for (int64_t id : fileIds_) {
                wolf::FileRecord rec;
                std::string err;
                wolf::RecoveryResult one;
                if (!wolf::loadRecoverRecord(engine_->getMetadataStore(), scanId_, id, rec, err)) {
                    one.success = false;
                    one.error = err;
                    summary_.results.push_back(one);
                    ++summary_.failed;
                    continue;
                }
                wolf::DiskReader reader;
                if (!wolf::bindReaderForRecord(reader, rec, driveIndex_, raid_, err)) {
                    one.success = false;
                    one.error = err;
                    summary_.results.push_back(one);
                    ++summary_.failed;
                    continue;
                }
                wolf::applyBoundFvek(reader, engine_->getDiskReader(), rec);
                one = recovery.recoverFile(reader, rec, destDir_);
                summary_.results.push_back(one);
                if (wolf::countsAsRecovered(one)) {
                    ++summary_.succeeded;
                    if (scanId_ > 0) engine_->getMetadataStore().incrementRecovered(scanId_);
                } else {
                    ++summary_.failed;
                }
            }
        } catch (const std::exception& e) {
            error_ = e.what();
        } catch (...) {
            error_ = "Unknown batch recovery error";
        }
    }

    void OnOK() override {
        Napi::Env env = Env();
        if (!error_.empty()) {
            deferred_.Reject(Napi::String::New(env, error_));
            return;
        }
        Napi::Object out = Napi::Object::New(env);
        out.Set("succeeded", Napi::Number::New(env, summary_.succeeded));
        out.Set("failed", Napi::Number::New(env, summary_.failed));
        Napi::Array arr = Napi::Array::New(env, summary_.results.size());
        for (size_t i = 0; i < summary_.results.size(); ++i) {
            arr[i] = recoveryResultToJs(env, summary_.results[i]);
        }
        out.Set("results", arr);
        deferred_.Resolve(out);
    }

    void OnError(const Napi::Error& e) override {
        deferred_.Reject(Napi::String::New(Env(), e.what()));
    }

private:
    wolf::Engine* engine_;
    int driveIndex_;
    std::vector<int64_t> fileIds_;
    std::string destDir_;
    int64_t scanId_ = -1;
    std::shared_ptr<wolf::VirtualRaid> raid_;
    Napi::Promise::Deferred deferred_;
    wolf::BatchRecoverySummary summary_;
    std::string error_;
};

Napi::Value RecoverFilesBatch(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 4 || !info[0].IsNumber() || !info[1].IsArray() ||
        !info[2].IsString() || !info[3].IsNumber()) {
        Napi::TypeError::New(env, "Expected driveIndex, fileIds[], destDir, scanId").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return env.Undefined();

    int driveIndex = info[0].As<Napi::Number>().Int32Value();
    Napi::Array arr = info[1].As<Napi::Array>();
    std::string destDir = info[2].As<Napi::String>().Utf8Value();
    int64_t scanId = info[3].As<Napi::Number>().Int64Value();

    std::vector<int64_t> ids;
    ids.reserve(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
        if (!arr.Get(i).IsNumber()) continue;
        ids.push_back(arr.Get(i).As<Napi::Number>().Int64Value());
    }
    if (ids.empty()) {
        Napi::TypeError::New(env, "No file ids to recover").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    BatchRecoverWorker* worker = new BatchRecoverWorker(env, &bdata->engine, driveIndex,
                                                        std::move(ids), destDir, scanId,
                                                        bdata->raid, deferred);
    worker->Queue();
    return deferred.Promise();
    NAPI_CATCH
}

Napi::Value ReadFilePreview(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
        Napi::TypeError::New(env, "Expected driveIndex, scanId, fileId").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return env.Undefined();

    int driveIndex = info[0].As<Napi::Number>().Int32Value();
    int64_t scanId = info[1].As<Napi::Number>().Int64Value();
    int64_t fileId = info[2].As<Napi::Number>().Int64Value();

    wolf::FileRecord rec;
    std::string err;
    if (!wolf::loadRecoverRecord(bdata->engine.getMetadataStore(), scanId, fileId, rec, err)) {
        Napi::Object out = Napi::Object::New(env);
        out.Set("success", Napi::Boolean::New(env, false));
        out.Set("error", Napi::String::New(env, err));
        return out;
    }

    wolf::DiskReader reader;
    if (!wolf::bindReaderForRecord(reader, rec, driveIndex, bdata->raid, err)) {
        Napi::Object out = Napi::Object::New(env);
        out.Set("success", Napi::Boolean::New(env, false));
        out.Set("error", Napi::String::New(env, err));
        return out;
    }
    wolf::applyBoundFvek(reader, bdata->engine.getDiskReader(), rec);

    wolf::FilePreviewResult preview = wolf::readFilePreview(reader, rec);
    Napi::Object out = Napi::Object::New(env);
    out.Set("success", Napi::Boolean::New(env, preview.success));
    out.Set("error", Napi::String::New(env, preview.error));
    out.Set("kind", Napi::String::New(env, preview.kind));
    if (preview.success && !preview.data.empty()) {
        out.Set("data", Napi::Buffer<uint8_t>::Copy(env, preview.data.data(), preview.data.size()));
    } else {
        out.Set("data", env.Null());
    }
    return out;
    NAPI_CATCH
}
