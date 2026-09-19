// bridge_drives.cpp — drive enumeration, partition tables, raw sector reads
// and SMART/health queries. See bridge_common.h for the shared context.
#include "bridge_common.h"
#include "byteback_carver.h"
#include "io/volume_mapper_win.h"
#include "io/hex_bind.h"
#include "io/byte_source.h"
#include "search/hex_search.h"
#include "fs/mft_record_view.h"

namespace {

std::string acceptedHexVolumePath(const std::string& vp) {
    if (byteback::isWin32VolumeDevicePath(vp)) return vp;
    if (vp.empty() || byteback::isHttpUrl(vp)) return {};
    if (vp.size() >= 4 && vp.compare(0, 4, "\\\\.\\") == 0) return {};
#ifndef _WIN32
    if (vp.size() >= 5 && vp.compare(0, 5, "/dev/") == 0) return {};
#endif
    return vp;
}

bool bindHexIo(byteback::DiskReader& reader, BridgeData* bdata, byteback::Engine* engine,
               int driveIndex, const std::string& volumePath, std::string& err) {
    byteback::FileRecord rec;
    std::shared_ptr<byteback::VirtualRaid> raid = bdata ? bdata->raid : nullptr;
    if (!byteback::bindReaderForRecord(reader, rec, driveIndex, raid, err, volumePath))
        return false;
    if (engine) reader.copyXtsFvekFrom(engine->getDiskReader());
    return true;
}

} // namespace

namespace {

// Whole-disk boot-sector search off the JS thread (TestDisk-style lost
// partition search). Progress rides a ThreadSafeFunction; the promise
// resolves with candidates incl. estimated sizes (start -> next candidate
// or disk end).
class LostPartitionsWorker : public Napi::AsyncWorker {
public:
    LostPartitionsWorker(Napi::Env& env, byteback::Engine* engine, int driveIndex,
                         uint32_t stepSectors, BridgeData* bdata,
                         Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), engine_(engine), driveIndex_(driveIndex),
          stepSectors_(stepSectors ? stepSectors : 512), bdata_(bdata), deferred_(deferred),
          tsfn_(Napi::ThreadSafeFunction::New(env, Napi::Function::New(env, [](const Napi::CallbackInfo&) { return; }),
                                              "LostPartitionsProgress", 0, 1, [](Napi::Env) {})) {}

    void Execute() override {
        try {
            byteback::DiskReader reader;
            if (!reader.openDrive(driveIndex_)) {
                error_ = "could not open PhysicalDrive";
                return;
            }
            byteback::PartitionScanner scanner(&reader);
            found_ = scanner.scanForPartitions(stepSectors_, [this](uint64_t cur, uint64_t total) {
                tsfn_.NonBlockingCall([cur, total](Napi::Env env, Napi::Function js) {
                    js.Call({Napi::Number::New(env, static_cast<double>(cur)),
                             Napi::Number::New(env, static_cast<double>(total))});
                });
            });
            unread_ = scanner.scanUnread();
            diskSectors_ = reader.getDiskSize() / (reader.getSectorSize() ? reader.getSectorSize() : 512);
        } catch (const std::exception& e) {
            error_ = e.what();
        } catch (...) {
            error_ = "unknown partition scan error";
        }
    }

    void OnOK() override {
        Napi::Env env = Env();
        tsfn_.Release();
        if (bdata_) bdata_->endHeavyOp();
        if (!error_.empty()) {
            deferred_.Reject(Napi::String::New(env, error_));
            return;
        }
        std::vector<byteback::PartitionInfo> sorted = found_;
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.startSector < b.startSector; });
        Napi::Array arr = Napi::Array::New(env, sorted.size());
        for (size_t i = 0; i < sorted.size(); ++i) {
            const uint64_t next = (i + 1 < sorted.size()) ? sorted[i + 1].startSector : diskSectors_;
            const uint64_t size = next > sorted[i].startSector ? next - sorted[i].startSector : 0;
            Napi::Object p = Napi::Object::New(env);
            p.Set("startSector", Napi::Number::New(env, static_cast<double>(sorted[i].startSector)));
            p.Set("sizeSectors", Napi::Number::New(env, static_cast<double>(size)));
            p.Set("fs", Napi::String::New(env, sorted[i].type.empty() ? "unknown" : sorted[i].type));
            arr[i] = p;
        }
        Napi::Object out = Napi::Object::New(env);
        out.Set("partitions", arr);
        out.Set("unread", Napi::Boolean::New(env, unread_));
        deferred_.Resolve(out);
    }

    void OnError(const Napi::Error& e) override {
        tsfn_.Release();
        if (bdata_) bdata_->endHeavyOp();
        deferred_.Reject(Napi::String::New(Env(), e.what()));
    }

private:
    byteback::Engine* engine_;
    int driveIndex_;
    uint32_t stepSectors_;
    BridgeData* bdata_;
    Napi::Promise::Deferred deferred_;
    Napi::ThreadSafeFunction tsfn_;
    std::vector<byteback::PartitionInfo> found_;
    uint64_t diskSectors_ = 0;
    bool unread_ = false;
    std::string error_;
};

class HexSearchWorker : public Napi::AsyncWorker {
public:
    HexSearchWorker(Napi::Env& env, byteback::Engine* engine, int driveIndex,
                    std::vector<uint8_t> needle, uint64_t maxHits,
                    std::string volumePath, BridgeData* bdata,
                    Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), engine_(engine), driveIndex_(driveIndex),
          needle_(std::move(needle)), maxHits_(maxHits), volumePath_(std::move(volumePath)),
          bdata_(bdata), deferred_(deferred) {}

    void Execute() override {
        try {
            byteback::DiskReader reader;
            std::string err;
            if (!bindHexIo(reader, bdata_, engine_, driveIndex_, volumePath_, err)) {
                error_ = err.empty() ? "Could not open volume device" : err;
                return;
            }
            std::atomic<bool> running{true};
            hits_ = byteback::searchRawBytes(reader, needle_.data(), needle_.size(),
                                             maxHits_, &running, &unread_);
        } catch (const std::exception& e) {
            error_ = e.what();
        } catch (...) {
            error_ = "unknown hex search error";
        }
    }

    void OnOK() override {
        Napi::Env env = Env();
        if (bdata_) bdata_->endHeavyOp();
        Napi::Object out = Napi::Object::New(env);
        Napi::Array arr = Napi::Array::New(env, hits_.size());
        for (size_t i = 0; i < hits_.size(); ++i) {
            arr[i] = Napi::Number::New(env, static_cast<double>(hits_[i].byteOffset));
        }
        out.Set("hits", arr);
        out.Set("unread", Napi::Boolean::New(env, unread_));
        if (!error_.empty()) out.Set("error", Napi::String::New(env, error_));
        deferred_.Resolve(out);
    }

    void OnError(const Napi::Error& e) override {
        if (bdata_) bdata_->endHeavyOp();
        deferred_.Reject(Napi::String::New(Env(), e.what()));
    }

private:
    byteback::Engine* engine_;
    int driveIndex_;
    std::vector<uint8_t> needle_;
    uint64_t maxHits_;
    std::string volumePath_;
    BridgeData* bdata_;
    Napi::Promise::Deferred deferred_;
    std::vector<byteback::HexSearchHit> hits_;
    bool unread_ = false;
    std::string error_;
};

} // namespace

Napi::Value ScanLostPartitions(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata || info.Length() < 1 || !info[0].IsNumber()) return env.Undefined();
    if (!bdata->tryBeginHeavyOp()) {
        Napi::Error::New(env, "Another disk operation is already running").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    const int driveIndex = info[0].As<Napi::Number>().Int32Value();
    const uint32_t stepSectors =
        (info.Length() >= 2 && info[1].IsNumber() && info[1].As<Napi::Number>().Uint32Value() > 0)
            ? info[1].As<Napi::Number>().Uint32Value()
            : 512;

    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    auto* worker = new LostPartitionsWorker(env, &bdata->engine, driveIndex, stepSectors, bdata, deferred);
    worker->Queue();
    return deferred.Promise();
    NAPI_CATCH
}

Napi::Value SearchHex(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata || info.Length() < 2 || !info[0].IsNumber() || !info[1].IsBuffer()) {
        Napi::TypeError::New(env, "Expected driveIndex and needle buffer").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    auto needleBuf = info[1].As<Napi::Buffer<uint8_t>>();
    if (needleBuf.Length() == 0 || needleBuf.Length() > byteback::kHexSearchMaxNeedle) {
        Napi::TypeError::New(env, "Needle length must be 1..64").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    if (!bdata->tryBeginHeavyOp()) {
        Napi::Error::New(env, "Another disk operation is already running").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    const int driveIndex = info[0].As<Napi::Number>().Int32Value();
    uint64_t maxHits = byteback::kHexSearchMaxHits;
    if (info.Length() >= 3 && info[2].IsNumber()) {
        const uint32_t n = info[2].As<Napi::Number>().Uint32Value();
        if (n > 0) maxHits = std::min(byteback::kHexSearchMaxHits, static_cast<uint64_t>(n));
    }
    std::string volumePath;
    if (info.Length() >= 4 && info[3].IsString()) {
        const std::string vp = info[3].As<Napi::String>().Utf8Value();
        volumePath = acceptedHexVolumePath(vp);
    }
    std::vector<uint8_t> needle(needleBuf.Data(), needleBuf.Data() + needleBuf.Length());
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    auto* worker = new HexSearchWorker(env, &bdata->engine, driveIndex, std::move(needle),
                                       maxHits, std::move(volumePath), bdata, deferred);
    worker->Queue();
    return deferred.Promise();
    NAPI_CATCH
}

Napi::Value GetMftRecord(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    byteback::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine || info.Length() < 2 || !info[0].IsNumber() || !info[1].IsNumber()) {
        return env.Undefined();
    }
    Napi::Object fail = Napi::Object::New(env);
    fail.Set("ok", Napi::Boolean::New(env, false));
    fail.Set("unread", Napi::Boolean::New(env, false));
    if (sharedReaderBusy(bdata)) {
        fail.Set("error", Napi::String::New(env, "Another disk operation is already running"));
        return fail;
    }

    const int driveIndex = info[0].As<Napi::Number>().Int32Value();
    const double refN = info[1].As<Napi::Number>().DoubleValue();
    if (!(refN >= 0) || refN > 1e15) {
        fail.Set("error", Napi::String::New(env, "Invalid MFT reference"));
        return fail;
    }
    const uint64_t mftRef = static_cast<uint64_t>(refN);
    std::string volumePath;
    if (info.Length() >= 3 && info[2].IsString()) {
        const std::string vp = info[2].As<Napi::String>().Utf8Value();
        volumePath = acceptedHexVolumePath(vp);
    }

    byteback::DiskReader diskReader;
    std::string bindErr;
    if (!bindHexIo(diskReader, bdata, engine, driveIndex, volumePath, bindErr)) {
        fail.Set("error", Napi::String::New(env, bindErr.empty() ? "Could not open volume device" : bindErr));
        return fail;
    }

    bool unread = false;
    auto view = byteback::getMftRecordView(diskReader, mftRef, 0, &unread);
    Napi::Object out = Napi::Object::New(env);
    out.Set("unread", Napi::Boolean::New(env, unread));
    if (!view) {
        out.Set("ok", Napi::Boolean::New(env, false));
        return out;
    }
    out.Set("ok", Napi::Boolean::New(env, true));
    out.Set("mftRef", Napi::Number::New(env, static_cast<double>(view->mftRef)));
    out.Set("byteOffset", Napi::Number::New(env, static_cast<double>(view->byteOffset)));
    out.Set("signature", jsUtf8(env, view->signature));
    out.Set("flags", Napi::Number::New(env, view->flags));
    Napi::Array attrs = Napi::Array::New(env, view->attrs.size());
    for (size_t i = 0; i < view->attrs.size(); ++i) {
        Napi::Object a = Napi::Object::New(env);
        a.Set("type", Napi::Number::New(env, view->attrs[i].type));
        a.Set("name", jsUtf8(env, view->attrs[i].name));
        a.Set("resident", Napi::Boolean::New(env, view->attrs[i].resident));
        attrs[i] = a;
    }
    out.Set("attrs", attrs);
    return out;
    NAPI_CATCH
}

Napi::Value GetCarveSignatureCount(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    return Napi::Number::New(env, static_cast<double>(byteback::CarvingEngine::globalSignatureCount()));
    NAPI_CATCH
}

Napi::Value GetVersion(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    byteback::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine) return Napi::String::New(env, "unknown");
    return Napi::String::New(env, engine->getVersion());
    NAPI_CATCH
}

Napi::Value IsAdministrator(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    byteback::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine) return Napi::Boolean::New(env, false);
    return Napi::Boolean::New(env, engine->isAdministrator());
    NAPI_CATCH
}

Napi::Value ListDrives(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    byteback::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine) return env.Undefined();

    auto drives = engine->getDiskReader().enumerateDrives();
    Napi::Array result = Napi::Array::New(env, drives.size());

    for (size_t i = 0; i < drives.size(); ++i) {
        Napi::Object drive = Napi::Object::New(env);
        drive.Set("index", Napi::Number::New(env, drives[i].index));
        drive.Set("model", jsUtf8(env, drives[i].model));
        drive.Set("serial", jsUtf8(env, drives[i].serial));
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
        Napi::TypeError::New(env, "Expected driveIndex").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    if (sharedReaderBusy(bdata)) {
        Napi::Error::New(env, "Another disk operation is already running").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    int driveIndex = info[0].As<Napi::Number>().Int32Value();
    byteback::DiskReader& reader = bdata->engine.getDiskReader();
    if (!reader.isOpen() || reader.getDriveIndex() != driveIndex) {
        if (!reader.openDrive(driveIndex)) {
            Napi::Error::New(env, "Cannot open drive for partition table").ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    byteback::PartitionScanner scanner(&reader);
    std::vector<byteback::PartitionInfo> parts = scanner.parseTables();
    if (scanner.tableUnread()) {
        byteback::PartitionInfo unread;
        unread.type = "table_unread";
        unread.startSector = 0;
        unread.sizeInSectors = 0;
        unread.isActive = false;
        parts.push_back(std::move(unread));
    }

    Napi::Array result = Napi::Array::New(env, parts.size());
    for (size_t i = 0; i < parts.size(); ++i) {
        Napi::Object p = Napi::Object::New(env);
        p.Set("type", jsUtf8(env, parts[i].type));
        p.Set("startSector", Napi::Number::New(env, static_cast<double>(parts[i].startSector)));
        p.Set("sizeInSectors", Napi::Number::New(env, static_cast<double>(parts[i].sizeInSectors)));
        p.Set("label", jsUtf8(env, parts[i].label));
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
    byteback::Engine* engine = bdata ? &bdata->engine : nullptr;
    if (!engine || info.Length() < 3) return env.Undefined();
    if (sharedReaderBusy(bdata)) {
        Napi::Object fail = Napi::Object::New(env);
        fail.Set("success", Napi::Boolean::New(env, false));
        fail.Set("bytesRead", Napi::Number::New(env, 0));
        fail.Set("paddedZeros", Napi::Boolean::New(env, false));
        fail.Set("error", Napi::String::New(env, "Another disk operation is already running"));
        return fail;
    }

    int driveIndex = info[0].As<Napi::Number>().Int32Value();
    double offset = info[1].As<Napi::Number>().DoubleValue();
    uint32_t size = info[2].As<Napi::Number>().Uint32Value();
    std::string volumePath;
    if (info.Length() >= 4 && info[3].IsString()) {
        const std::string vp = info[3].As<Napi::String>().Utf8Value();
        volumePath = acceptedHexVolumePath(vp);
    }

    constexpr uint32_t kMaxRead = 1024 * 1024;
    if (size == 0 || size > kMaxRead) {
        Napi::TypeError::New(env, "Read size must be between 1 and 1048576 bytes").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    byteback::DiskReader diskReader;
    std::string bindErr;
    if (!bindHexIo(diskReader, bdata, engine, driveIndex, volumePath, bindErr)) {
        Napi::Object fail = Napi::Object::New(env);
        fail.Set("success", Napi::Boolean::New(env, false));
        fail.Set("bytesRead", Napi::Number::New(env, 0));
        fail.Set("paddedZeros", Napi::Boolean::New(env, false));
        fail.Set("error", Napi::String::New(env, bindErr.empty() ? "Could not open volume device" : bindErr));
        return fail;
    }

    uint8_t* buffer = static_cast<uint8_t*>(_aligned_malloc(size, 4096));
    if (!buffer) return env.Undefined();

    auto result = diskReader.readSectors(static_cast<uint64_t>(offset), size, buffer);

    Napi::Object obj = Napi::Object::New(env);
    obj.Set("success", Napi::Boolean::New(env, result.success && !result.paddedZeros));
    obj.Set("bytesRead", Napi::Number::New(env, static_cast<double>(result.bytesRead)));
    obj.Set("paddedZeros", Napi::Boolean::New(env, result.paddedZeros));
    obj.Set("error", Napi::String::New(env, result.paddedZeros ? "short or padded read" : result.error));

    if (result.success && !result.paddedZeros && result.bytesRead > 0) {
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

    byteback::SmartMonitor monitor;
    auto status = monitor.getSmartStatus(driveIndex);

    Napi::Object obj = Napi::Object::New(env);
    obj.Set("isValid", Napi::Boolean::New(env, status.isValid));
    // Seek-penalty / SSD bits are independent of health-log validity.
    // Unread SMART must not look like "confirmed HDD" on the JS side.
    obj.Set("isSsd", Napi::Boolean::New(env, status.isSsd));
    obj.Set("seekPenaltyKnown", Napi::Boolean::New(env, status.seekPenaltyKnown));
    if (status.isValid) {
        obj.Set("driveModel", jsUtf8(env, status.driveModel));
        obj.Set("healthScore", jsUtf8(env, status.healthScore));
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
    }
    return obj;
    NAPI_CATCH
}

Napi::Value ResolveVolume(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 1 || !info[0].IsString()) return env.Null();

    std::string letter = info[0].As<Napi::String>().Utf8Value();
    auto norm = byteback::normalizeDriveLetterUtf8(letter);
    if (!norm) return env.Null();

#ifndef _WIN32
    return env.Null();
#else
    auto resolved = byteback::resolveDriveLetter(*norm);
    if (!resolved) return env.Null();

    Napi::Object obj = Napi::Object::New(env);
    obj.Set("driveIndex", Napi::Number::New(env, resolved->driveIndex));
    obj.Set("startSector", Napi::Number::New(env, static_cast<double>(resolved->partitionStartSector)));
    obj.Set("sizeSectors", Napi::Number::New(env, static_cast<double>(resolved->partitionSizeSectors)));
    obj.Set("fsType", Napi::String::New(env, byteback::volumeFsKindLabel(resolved->fsKind)));
    obj.Set("diskExtentCount", Napi::Number::New(env, resolved->diskExtentCount));
    if (!resolved->volumePath.empty())
        obj.Set("volumePath", Napi::String::New(env, resolved->volumePath));
    Napi::Array disks = Napi::Array::New(env, resolved->diskNumbers.size());
    for (size_t i = 0; i < resolved->diskNumbers.size(); ++i)
        disks[i] = Napi::Number::New(env, resolved->diskNumbers[i]);
    obj.Set("diskNumbers", disks);
    return obj;
#endif
    NAPI_CATCH
}

Napi::Value ListVolumeLetters(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
#ifndef _WIN32
    return Napi::Array::New(env, 0);
#else
    auto letters = byteback::listLogicalDriveLetters();
    Napi::Array result = Napi::Array::New(env, letters.size());
    for (size_t i = 0; i < letters.size(); ++i) {
        char narrow[4] = {
            static_cast<char>(letters[i][0]),
            ':',
            '\0',
        };
        result[i] = Napi::String::New(env, narrow);
    }
    return result;
#endif
    NAPI_CATCH
}
