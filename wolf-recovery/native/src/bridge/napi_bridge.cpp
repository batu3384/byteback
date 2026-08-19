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
    exports.Set("searchFiles", Napi::Function::New(env, SearchFiles));
    exports.Set("searchFileContent", Napi::Function::New(env, SearchFileContent));
    exports.Set("startContentSearch", Napi::Function::New(env, StartContentSearch));
    exports.Set("stopContentSearch", Napi::Function::New(env, StopContentSearch));
    exports.Set("getScanSummary", Napi::Function::New(env, GetScanSummary));
    exports.Set("startScan", Napi::Function::New(env, StartScan));
    exports.Set("stopScan", Napi::Function::New(env, StopScan));

    // bridge_imager.cpp
    exports.Set("startImaging", Napi::Function::New(env, StartImaging));
    exports.Set("stopImaging", Napi::Function::New(env, StopImaging));

    // bridge_wipe.cpp
    exports.Set("startWipe", Napi::Function::New(env, StartWipe));
    exports.Set("reconstructRaid", Napi::Function::New(env, ReconstructRaid));
    exports.Set("getRaidState", Napi::Function::New(env, GetRaidState));
    exports.Set("recoverFile", Napi::Function::New(env, RecoverFile));
    exports.Set("recoverFilesBatch", Napi::Function::New(env, RecoverFilesBatch));

    // bridge_ops.cpp
    exports.Set("getCaseInfo", Napi::Function::New(env, GetCaseInfo));
    exports.Set("setCaseInfo", Napi::Function::New(env, SetCaseInfo));
    exports.Set("loadNsrl", Napi::Function::New(env, LoadNsrl));
    exports.Set("lookupNsrl", Napi::Function::New(env, LookupNsrl));
    exports.Set("getNsrlStats", Napi::Function::New(env, GetNsrlStats));

    return exports;
}

NODE_API_MODULE(wolf_engine, Init)
