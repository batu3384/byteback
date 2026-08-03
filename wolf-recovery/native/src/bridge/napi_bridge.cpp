#include <napi.h>
#include "wolf_engine.h"
#include <cstdlib> // For _aligned_malloc and _aligned_free


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

Napi::Value ListDrives(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    wolf::Engine* engine = env.GetInstanceData<wolf::Engine>();
    if (!engine) {
        Napi::Error::New(env, "Engine not initialized").ThrowAsJavaScriptException();
        return env.Undefined();
    }

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
    wolf::Engine* engine = env.GetInstanceData<wolf::Engine>();
    if (!engine) {
        Napi::Error::New(env, "Engine not initialized").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (info.Length() < 3) {
        Napi::TypeError::New(env, "Wrong number of arguments: expected driveIndex, offset, and size").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (!info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
        Napi::TypeError::New(env, "Invalid argument types: expected numbers for driveIndex, offset, and size").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    int driveIndex = info[0].As<Napi::Number>().Int32Value();
    double offset = info[1].As<Napi::Number>().DoubleValue();
    uint32_t size = info[2].As<Napi::Number>().Uint32Value();

    auto& diskReader = engine->getDiskReader();
    if (!diskReader.isOpen() || diskReader.getDriveIndex() != driveIndex) {
        diskReader.openDrive(driveIndex);
    }

    // Allocate sector-aligned buffer
    uint8_t* buffer = static_cast<uint8_t*>(_aligned_malloc(size, 4096));
    if (!buffer) {
        Napi::Error::New(env, "Failed to allocate aligned buffer").ThrowAsJavaScriptException();
        return env.Undefined();
    }

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

Napi::Value InitDatabase(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    wolf::Engine* engine = env.GetInstanceData<wolf::Engine>();
    if (!engine) {
        Napi::Error::New(env, "Engine not initialized").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "Expected a string argument for database path").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string dbPath = info[0].As<Napi::String>().Utf8Value();
    bool ok = engine->getMetadataStore().open(dbPath);
    return Napi::Boolean::New(env, ok);
}

Napi::Value GetFileCount(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    wolf::Engine* engine = env.GetInstanceData<wolf::Engine>();
    if (!engine) {
        Napi::Error::New(env, "Engine not initialized").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (info.Length() < 1 || !info[0].IsNumber()) {
        Napi::TypeError::New(env, "Expected a number argument for scan ID").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    int64_t scanId = info[0].As<Napi::Number>().Int64Value();
    int64_t count = engine->getMetadataStore().getFileCount(scanId);
    return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    env.SetInstanceData<wolf::Engine>(new wolf::Engine());

    exports.Set("getVersion", Napi::Function::New(env, GetVersion));
    exports.Set("isAdministrator", Napi::Function::New(env, IsAdministrator));
    exports.Set("listDrives", Napi::Function::New(env, ListDrives));
    exports.Set("readSectors", Napi::Function::New(env, ReadSectors));
    exports.Set("initDatabase", Napi::Function::New(env, InitDatabase));
    exports.Set("getFileCount", Napi::Function::New(env, GetFileCount));

    return exports;
}

NODE_API_MODULE(wolf_engine, Init)
