// bridge_imager.cpp — disk imaging (raw/dd and EWF/E01 with on-the-fly MD5).
// See bridge_common.h for the shared context.
#include "bridge_common.h"
#include "io/volume_mapper_win.h"

Napi::Value StartImaging(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsString() || !info[2].IsFunction()) {
        Napi::TypeError::New(env, "Expected driveIndex, destPath, callback, [format, volumePath]").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    BridgeData* bdata = env.GetInstanceData<BridgeData>();
    if (!bdata) return env.Undefined();

    if (bdata->imagerContext) {
        bdata->imagerContext->imager.stopImaging();
        bdata->imagerContext.reset();
        bdata->endHeavyOp();
    }

    if (!bdata->tryBeginHeavyOp()) {
        Napi::Error::New(env, "Another disk operation is already running").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    int driveIndex = info[0].As<Napi::Number>().Int32Value();
    std::string destPath = info[1].As<Napi::String>().Utf8Value();
    Napi::Function cb = info[2].As<Napi::Function>();
    // Optional 4th arg: format string — "ewf" selects Expert Witness (.E01)
    // with on-the-fly MD5; anything else (or absent) is raw/dd.
    byteback::ImageFormat format = byteback::ImageFormat::Raw;
    if (info.Length() >= 4 && info[3].IsString()) {
        std::string f = info[3].As<Napi::String>().Utf8Value();
        if (f == "ewf" || f == "e01") format = byteback::ImageFormat::Ewf;
    }
    std::string volumePath;
    if (info.Length() >= 5 && info[4].IsString()) {
        const std::string vp = info[4].As<Napi::String>().Utf8Value();
        if (byteback::isWin32VolumeDevicePath(vp)) volumePath = vp;
    }

    if (driveIndex < 0 && driveIndex != -1) {
        bdata->endHeavyOp();
        return Napi::Boolean::New(env, false);
    }
    if (driveIndex == -1 && !bdata->raid) {
        bdata->endHeavyOp();
        return Napi::Boolean::New(env, false);
    }
    if (driveIndex == -1) volumePath.clear();

    auto context = std::make_shared<ImagerContext>();
    bdata->imagerContext = context;

    context->tsfn = Napi::ThreadSafeFunction::New(
        env, cb, "ImagingCallback", 0, 1,
        [](Napi::Env) {}
    );

    byteback::EwfOptions ewfOpts;
    byteback::CaseInfo caseInfo = bdata->engine.getMetadataStore().getCaseInfo();
    if (!caseInfo.caseNumber.empty()) ewfOpts.caseNumber = caseInfo.caseNumber;
    if (!caseInfo.investigator.empty()) ewfOpts.examiner = caseInfo.investigator;
    else ewfOpts.examiner = "Byteback";
    if (!caseInfo.notes.empty()) ewfOpts.notes = caseInfo.notes;
    if (!caseInfo.agency.empty()) {
        if (!ewfOpts.notes.empty()) ewfOpts.notes += " | ";
        ewfOpts.notes += "Agency: " + caseInfo.agency;
    }

    auto lastProgress = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    auto imagerPtr = &(context->imager);
    auto onProgress = [context, lastProgress, imagerPtr, bdata](uint64_t current, uint64_t total) {
        auto now = std::chrono::steady_clock::now();
        if (current != total && std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastProgress).count() < 100) {
            return;
        }
        *lastProgress = now;

        auto callback = [current, total, imagerPtr](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object obj = Napi::Object::New(env);
            obj.Set("type", Napi::String::New(env, "progress"));
            obj.Set("current", Napi::Number::New(env, static_cast<double>(current)));
            obj.Set("total", Napi::Number::New(env, static_cast<double>(total)));
            // CA-007: read-failure telemetry for the imaging session.
            obj.Set("badSectors", Napi::Number::New(env, static_cast<double>(imagerPtr->badSectorReads())));
            // On completion, surface the image MD5 (EWF images are hashed on
            // the fly; empty for raw images).
            if (current >= total) {
                obj.Set("md5", Napi::String::New(env, imagerPtr->lastImageMd5()));
            }
            jsCallback.Call({obj});
        };
        tsfnPost(context->tsfn, callback);
        if (current >= total) {
            context->tsfn.Release();
            bdata->endHeavyOp();
        }
    };

    forensic::AuditLogger::GetInstance().LogEvent(
        "IMAGING_START | drive=" + std::to_string(driveIndex) +
        " | dest=" + destPath + " | format=" + (format == byteback::ImageFormat::Ewf ? "ewf" : "raw") +
        (driveIndex == -1 ? " | raid=1" : "") +
        (volumePath.empty() ? "" : " | volume=" + volumePath));

    if (driveIndex == -1) {
        context->raidReader.setRaidBackend(bdata->raid);
        context->raidReader.copyXtsFvekFrom(bdata->engine.getDiskReader());
        context->imager.startImagingFromReader(context->raidReader, destPath, onProgress, format, ewfOpts);
    } else {
        context->imager.startImaging(driveIndex, destPath, onProgress, format, ewfOpts, volumePath);
    }

    return Napi::Boolean::New(env, true);
    NAPI_CATCH
}

Napi::Value StopImaging(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    NAPI_TRY
    BridgeData* bdata = env.GetInstanceData<BridgeData>();

    if (bdata && bdata->imagerContext) {
        bdata->imagerContext->imager.requestStop();
    }
    return env.Undefined();
    NAPI_CATCH
}
