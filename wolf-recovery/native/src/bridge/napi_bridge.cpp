// napi_bridge.cpp — thin registration unit (CA-013 split). The handlers live
// in bridge_drives.cpp / bridge_scan.cpp / bridge_imager.cpp /
// bridge_wipe.cpp and are declared in bridge_common.h; this file only owns
// Init() and the module registration.
#include "bridge_common.h"

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    env.SetInstanceData<BridgeData>(new BridgeData());

    // bridge_drives.cpp
    exports.Set("getVersion", Napi::Function::New(env, GetVersion));
    exports.Set("isAdministrator", Napi::Function::New(env, IsAdministrator));
    exports.Set("listDrives", Napi::Function::New(env, ListDrives));
    exports.Set("listPartitions", Napi::Function::New(env, ListPartitions));
    exports.Set("readSectors", Napi::Function::New(env, ReadSectors));
    exports.Set("getSmartStatus", Napi::Function::New(env, GetSmartStatus));

    // bridge_scan.cpp
    exports.Set("initDatabase", Napi::Function::New(env, InitDatabase));
    exports.Set("getFileCount", Napi::Function::New(env, GetFileCount));
    exports.Set("getFilesPage", Napi::Function::New(env, GetFilesPage));
    exports.Set("getScanState", Napi::Function::New(env, GetScanState));
    exports.Set("getLatestScanId", Napi::Function::New(env, GetLatestScanId));
    exports.Set("getTimelineEvents", Napi::Function::New(env, GetTimelineEvents));
    exports.Set("getAuditLog", Napi::Function::New(env, GetAuditLog));
    exports.Set("startScan", Napi::Function::New(env, StartScan));
    exports.Set("stopScan", Napi::Function::New(env, StopScan));

    // bridge_imager.cpp
    exports.Set("startImaging", Napi::Function::New(env, StartImaging));
    exports.Set("stopImaging", Napi::Function::New(env, StopImaging));

    // bridge_wipe.cpp
    exports.Set("startWipe", Napi::Function::New(env, StartWipe));
    exports.Set("reconstructRaid", Napi::Function::New(env, ReconstructRaid));
    exports.Set("recoverFile", Napi::Function::New(env, RecoverFile));

    return exports;
}

NODE_API_MODULE(wolf_engine, Init)
