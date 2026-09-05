// CA-039: shell-free process helpers. quoteProcessArg must produce argv-safe
// quoting for adversarial inputs; runProcess must honor the exit code and
// timeout contract.
#include "recovery/process_util.h"
#include <gtest/gtest.h>

#ifdef _WIN32

#include <direct.h>
#include <filesystem>
#include <fstream>
#include <stdlib.h>
#include <string>

using byteback::quoteProcessArg;
using byteback::runProcess;
using byteback::resolveOnPath;
using byteback::resolveOnPathW;

TEST(ProcessUtil, QuotesArgumentsPerArgvRules) {
    EXPECT_EQ(quoteProcessArg("plain"), "plain");
    EXPECT_EQ(quoteProcessArg("-hide_banner"), "-hide_banner");
    EXPECT_EQ(quoteProcessArg(""), "\"\"");
    EXPECT_EQ(quoteProcessArg("with space"), "\"with space\"");
    // A literal quote inside a quoted arg: one backslash + quote.
    EXPECT_EQ(quoteProcessArg("say \"hi\""), "\"say \\\"hi\\\"\"");
    // Backslash runs before a quote double, then one escapes the quote.
    EXPECT_EQ(quoteProcessArg("a\\\"b"), "\"a\\\\\\\"b\"");
    // Trailing backslashes before the closing quote double up.
    EXPECT_EQ(quoteProcessArg("C:\\temp dir\\"), "\"C:\\temp dir\\\\\"");
    EXPECT_EQ(quoteProcessArg("C:\\normal\\path"), "C:\\normal\\path");
}

TEST(ProcessUtil, RunProcessHonorsExitCode) {
    EXPECT_TRUE(runProcess("cmd", {"/c", "exit", "0"}, 10000));
    EXPECT_FALSE(runProcess("cmd", {"/c", "exit", "3"}, 10000));
    EXPECT_FALSE(runProcess("definitely-not-a-real-exe-xyz", {}, 5000));
}

TEST(ProcessUtil, ResolveOnPath) {
    EXPECT_TRUE(resolveOnPath("definitely-not-a-real-exe-xyz").empty());
    // cmd.exe is guaranteed on PATH for the Windows runners.
    EXPECT_NE(resolveOnPath("cmd"), "");
}

// F2: a relative PATH entry resolves against the CWD — a planter only needs
// to drop ffmpeg.exe in a directory the user later launches from. Only
// absolute entries may be searched.
TEST(ProcessUtil, ResolveOnPathSkipsRelativeEntries) {
    const auto temp = std::filesystem::temp_directory_path() / "byteback_path_test";
    std::filesystem::create_directories(temp / "plant");
    { std::ofstream f(temp / "plant" / "ffmpeg.exe", std::ios::binary); f << 'M'; }

    wchar_t oldCwd[1024];
    EXPECT_NE(_wgetcwd(oldCwd, 1024), nullptr);
    const std::wstring savedPath = _wgetenv(L"PATH") ? _wgetenv(L"PATH") : L"";
    _wputenv(L"PATH=plant");
    EXPECT_EQ(_wchdir(temp.wstring().c_str()), 0);

    // Relative "plant" + CWD contains plant\ffmpeg.exe — must NOT resolve.
    EXPECT_TRUE(resolveOnPathW(L"ffmpeg").empty());

    // Absolute entry (with a trailing backslash, as seen in real PATHs) works.
    _wputenv((L"PATH=" + (temp / "plant").wstring() + L"\\").c_str());
    EXPECT_FALSE(resolveOnPathW(L"ffmpeg").empty());
    // A name that already carries .exe must not become ".exe.exe".
    EXPECT_FALSE(resolveOnPathW(L"ffmpeg.exe").empty());

    EXPECT_EQ(_wchdir(oldCwd), 0);
    _wputenv((L"PATH=" + savedPath).c_str());
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
}

#endif // _WIN32
