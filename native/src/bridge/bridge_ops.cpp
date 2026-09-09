// bridge_ops.cpp — forensic ops: case metadata + NSRL hash lookup + CSV export.
#include "bridge_common.h"

namespace {

// FAZ 1.3c: streaming CSV export off the JS thread (a multi-million-row scan
// would freeze the window on a sync call). Mirrors LostPartitionsWorker's
// promise style; progress is not required for v1 — rowsOut travels at the end.
class ExportCsvWorker : public Napi::AsyncWorker {
public:
    ExportCsvWorker(Napi::Env& env, byteback::Engine* engine, int64_t scanId,
                    std::string destPath, byteback::FileListFilter filter,
                    std::vector<std::string> header, std::string noFsDate, std::string noDate,
                    Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), engine_(engine), scanId_(scanId),
          destPath_(std::move(destPath)), filter_(std::move(filter)), header_(std::move(header)),
          noFsDate_(std::move(noFsDate)), noDate_(std::move(noDate)), deferred_(deferred) {}

    void Execute() override {
        try {
            ok_ = engine_->getMetadataStore().exportCsv(scanId_, destPath_, filter_, header_,
                                                        noFsDate_, noDate_, &rows_, &error_);
        } catch (const std::exception& e) {
            ok_ = false;
            error_ = e.what();
        } catch (...) {
            ok_ = false;
            error_ = "unknown CSV export error";
        }
    }

    void OnOK() override {
        if (!ok_) {
            deferred_.Reject(Napi::String::New(Env(), error_));
            return;
        }
        Napi::Object obj = Napi::Object::New(Env());
        obj.Set("ok", Napi::Boolean::New(Env(), true));
        obj.Set("rows", Napi::Number::New(Env(), static_cast<double>(rows_)));
        deferred_.Resolve(obj);
    }

    void OnError(const Napi::Error& e) override {
        deferred_.Reject(Napi::String::New(Env(), e.what()));
    }

private:
    byteback::Engine* engine_;
    int64_t scanId_;
    std::string destPath_;
    byteback::FileListFilter filter_;
    std::vector<std::string> header_;
    std::string noFsDate_;
    std::string noDate_;
    Napi::Promise::Deferred deferred_;
    bool ok_ = false;
    int64_t rows_ = 0;
    std::string error_;
};

} // namespace

// Args: (scanId:int64, destPath:string, filter:object, header:string[10],
// noFsDate:string, noDate:string) -> Promise<{ ok: true, rows: number }>.
// destPath is dialog-arbitrated by the main process (imaging allowlist
// pattern): the renderer never supplies a write path across this boundary.
Napi::Value ExportCsv(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata || info.Length() < 6 || !info[0].IsNumber() || !info[1].IsString() ||
        !info[2].IsObject() || !info[3].IsArray() || !info[4].IsString() || !info[5].IsString()) {
        Napi::TypeError::New(env, "Expected (scanId, destPath, filter, header[10], noFsDate, noDate)")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }

    const int64_t scanId = info[0].As<Napi::Number>().Int64Value();
    const std::string destPath = info[1].As<Napi::String>().Utf8Value();
    if (scanId <= 0 || destPath.empty()) {
        Napi::TypeError::New(env, "exportCsv expects a positive scanId and a destination path")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }

    const Napi::Array headerArr = info[3].As<Napi::Array>();
    std::vector<std::string> header;
    header.reserve(headerArr.Length());
    for (uint32_t i = 0; i < headerArr.Length(); ++i) {
        if (!headerArr.Get(i).IsString()) {
            Napi::TypeError::New(env, "exportCsv header entries must be strings")
                .ThrowAsJavaScriptException();
            return env.Undefined();
        }
        header.push_back(headerArr.Get(i).As<Napi::String>().Utf8Value());
    }

    byteback::FileListFilter filter = FilterFromJs(info[2]);
    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    auto* worker = new ExportCsvWorker(env, &bdata->engine, scanId, destPath, filter,
                                       std::move(header),
                                       info[4].As<Napi::String>().Utf8Value(),
                                       info[5].As<Napi::String>().Utf8Value(),
                                       deferred);
    worker->Queue();
    return deferred.Promise();
    NAPI_CATCH
}

Napi::Value GetCaseInfo(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return env.Undefined();

    byteback::CaseInfo c = bdata->engine.getMetadataStore().getCaseInfo();
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("caseNumber", Napi::String::New(env, c.caseNumber));
    obj.Set("investigator", Napi::String::New(env, c.investigator));
    obj.Set("agency", Napi::String::New(env, c.agency));
    obj.Set("notes", Napi::String::New(env, c.notes));
    obj.Set("createdAt", Napi::Number::New(env, static_cast<double>(c.createdAt)));
    obj.Set("updatedAt", Napi::Number::New(env, static_cast<double>(c.updatedAt)));
    return obj;
    NAPI_CATCH
}

Napi::Value SetCaseInfo(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata || info.Length() < 1 || !info[0].IsObject()) return env.Undefined();

    Napi::Object obj = info[0].As<Napi::Object>();
    byteback::CaseInfo c = bdata->engine.getMetadataStore().getCaseInfo();
    // Guarded reads: a non-string field from a future caller must be skipped,
    // not thrown into NAPI_CATCH and reported as a failed save.
    auto getString = [&obj](const char* key, std::string& out) {
        if (obj.Has(key) && obj.Get(key).IsString()) out = obj.Get(key).As<Napi::String>().Utf8Value();
    };
    getString("caseNumber", c.caseNumber);
    getString("investigator", c.investigator);
    getString("agency", c.agency);
    getString("notes", c.notes);

    bool ok = bdata->engine.getMetadataStore().setCaseInfo(c);
    if (ok) {
        forensic::AuditLogger::GetInstance().LogEvent(
            "CASE_UPDATE | number=" + c.caseNumber + " | investigator=" + c.investigator);
    }
    return Napi::Boolean::New(env, ok);
    NAPI_CATCH
}

// CA-028: JS-origin audit events. Args: (event: string) — non-empty, <=512
// chars, UPPERCASE/underscore token first, printable ASCII payload. Rejected
// input throws; accepted input is written through LogEventFromBridge, which
// tags it "JS" in the hash-chained log line.
Napi::Value LogAuditEvent(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "logAuditEvent expects a string event").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    const std::string event = info[0].As<Napi::String>().Utf8Value();
    if (event.empty()) {
        Napi::TypeError::New(env, "logAuditEvent: event must not be empty").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    if (event.size() > 512) {
        Napi::TypeError::New(env, "logAuditEvent: event exceeds 512 characters").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    bool ok = forensic::AuditLogger::GetInstance().LogEventFromBridge(event);
    if (!ok) {
        Napi::TypeError::New(
            env, "logAuditEvent: invalid event (expected 'UPPERCASE_TOKEN | key=value' ASCII)")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }
    return Napi::Boolean::New(env, true);
    NAPI_CATCH
}

Napi::Value LoadNsrl(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata || info.Length() < 1 || !info[0].IsString()) return env.Undefined();

    std::string path = info[0].As<Napi::String>().Utf8Value();
    bool ok = bdata->nsrl.loadFromFile(path);
    if (ok) {
        forensic::AuditLogger::GetInstance().LogEvent(
            "NSRL_LOAD | path=" + path + " | count=" + std::to_string(bdata->nsrl.size()));
    }
    Napi::Object result = Napi::Object::New(env);
    result.Set("ok", Napi::Boolean::New(env, ok));
    result.Set("count", Napi::Number::New(env, static_cast<double>(bdata->nsrl.size())));
    result.Set("path", Napi::String::New(env, bdata->nsrl.lastPath()));
    return result;
    NAPI_CATCH
}

Napi::Value LookupNsrl(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata || info.Length() < 1 || !info[0].IsString()) return env.Undefined();

    std::string md5 = info[0].As<Napi::String>().Utf8Value();
    return Napi::Boolean::New(env, bdata->nsrl.contains(md5));
    NAPI_CATCH
}

Napi::Value GetNsrlStats(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return env.Undefined();

    Napi::Object obj = Napi::Object::New(env);
    obj.Set("count", Napi::Number::New(env, static_cast<double>(bdata->nsrl.size())));
    obj.Set("path", Napi::String::New(env, bdata->nsrl.lastPath()));
    return obj;
    NAPI_CATCH
}

// CA-010: main process hands over the absolute signatures directory (packaged
// apps cannot read resources inside app.asar via std::ifstream).
Napi::Value SetSignaturesDir(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 1 || !info[0].IsString()) return Napi::Boolean::New(env, false);
    byteback::CarvingEngine::setResourceSignatureDir(info[0].As<Napi::String>().Utf8Value());
    return Napi::Boolean::New(env, true);
    NAPI_CATCH
}

// P0-3: user signature overlay file (resource-format JSON) on top of built-ins.
Napi::Value SetSignatureOverlay(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 1 || !info[0].IsString()) return Napi::Boolean::New(env, false);
    const std::string path = info[0].As<Napi::String>().Utf8Value();
    if (path.empty()) {
        byteback::CarvingEngine::setSignatureOverlay("");
        return Napi::Boolean::New(env, true);
    }
    std::ifstream probe(path);
    if (!probe.is_open()) return Napi::Boolean::New(env, false);
    byteback::CarvingEngine::setSignatureOverlay(path);
    return Napi::Boolean::New(env, true);
    NAPI_CATCH
}
