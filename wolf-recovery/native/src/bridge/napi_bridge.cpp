#include <napi.h>
#include "wolf_engine.h"

static wolf::Engine* g_engine = nullptr;

Napi::Value GetVersion(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!g_engine) {
        Napi::Error::New(env, "Engine not initialized").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    return Napi::String::New(env, g_engine->getVersion());
}

Napi::Value IsAdministrator(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!g_engine) {
        Napi::Error::New(env, "Engine not initialized").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    return Napi::Boolean::New(env, g_engine->isAdministrator());
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    g_engine = new wolf::Engine();

    exports.Set("getVersion", Napi::Function::New(env, GetVersion));
    exports.Set("isAdministrator", Napi::Function::New(env, IsAdministrator));

    return exports;
}

NODE_API_MODULE(wolf_engine, Init)
