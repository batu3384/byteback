// bridge_ops.cpp — forensic ops: case metadata + NSRL hash lookup.
#include "bridge_common.h"

Napi::Value GetCaseInfo(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return env.Undefined();

    wolf::CaseInfo c = bdata->engine.getMetadataStore().getCaseInfo();
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
    wolf::CaseInfo c = bdata->engine.getMetadataStore().getCaseInfo();
    if (obj.Has("caseNumber")) c.caseNumber = obj.Get("caseNumber").As<Napi::String>().Utf8Value();
    if (obj.Has("investigator")) c.investigator = obj.Get("investigator").As<Napi::String>().Utf8Value();
    if (obj.Has("agency")) c.agency = obj.Get("agency").As<Napi::String>().Utf8Value();
    if (obj.Has("notes")) c.notes = obj.Get("notes").As<Napi::String>().Utf8Value();

    bool ok = bdata->engine.getMetadataStore().setCaseInfo(c);
    if (ok) {
        forensic::AuditLogger::GetInstance().LogEvent(
            "CASE_UPDATE | number=" + c.caseNumber + " | investigator=" + c.investigator);
    }
    return Napi::Boolean::New(env, ok);
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
