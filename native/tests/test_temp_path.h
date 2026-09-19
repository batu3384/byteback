#pragma once

#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

// FAZ 1.4: parallel gtest_discover / multi-lane ctest share %TEMP%.
inline int bytebackTestPid() {
#if defined(_WIN32)
    return ::_getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

inline std::filesystem::path bytebackTestTemp(const std::string& stem,
                                              const std::string& ext = {}) {
    return std::filesystem::temp_directory_path() /
           (stem + "_" + std::to_string(bytebackTestPid()) + ext);
}
