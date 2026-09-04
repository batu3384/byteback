// CA-039: shell-free process helpers. quoteProcessArg must produce argv-safe
// quoting for adversarial inputs; runProcess must honor the exit code and
// timeout contract.
#include "recovery/process_util.h"
#include <gtest/gtest.h>

#ifdef _WIN32

using byteback::quoteProcessArg;
using byteback::runProcess;
using byteback::resolveOnPath;

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

#endif // _WIN32
