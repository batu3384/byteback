#include <napi.h>
#include "wolf_engine.h"

Napi::Value GetVersion(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    wolf::Engine* engine = env.GetInstanceData<wolf::Engine>();
    if (!engine) {
        Napi::Error::New(env, "Engine not initialized").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    return Napi::String::New(env, engine->getVersion());
}

Napi::Value IsAdministrator(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    wolf::Engine* engine = env.GetInstanceData<wolf::Engine>();
    if (!engine) {
        Napi::Error::New(env, "Engine not initialized").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    return Napi::Boolean::New(env, engine->isAdministrator());
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    env.SetInstanceData<wolf::Engine>(new wolf::Engine());

    exports.Set("getVersion", Napi::Function::New(env, GetVersion));
    exports.Set("isAdministrator", Napi::Function::New(env, IsAdministrator));

    return exports;
}

NODE_API_MODULE(wolf_engine, Init)
