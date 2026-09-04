#pragma once

// CA-039/AR2: shell-free child process execution. CreateProcessW takes the
// command line directly: no shell, and every argument goes through argv-rule
// quoting. Wide-char end to end (F5): non-ASCII temp paths must not round-trip
// through the system code page. resolveOnPathW scans PATH entries only (F2:
// SearchPathW's app-dir/CWD-first order enabled binary planting) and only ever
// resolves .exe targets (F3: .bat/.cmd would respawn cmd.exe).

#include <cstdint>
#include <string>
#include <vector>

namespace byteback {

#ifdef _WIN32

/** UTF-8 -> UTF-16. */
std::wstring utf8ToWide(const std::string& s);

/** Quote one argument per Windows argv parsing rules (wide variant). */
std::wstring quoteProcessArgW(const std::wstring& arg);

/**
 * Run `exe` with `args` (no shell, hidden window). True iff launched, exited
 * within timeoutMs with code 0; overruns are terminated.
 */
bool runProcessW(const std::wstring& exe, const std::vector<std::wstring>& args, uint32_t timeoutMs);

/**
 * Resolve an executable to an .exe file. Explicit paths must already end in
 * .exe; bare names are searched across PATH entries only (no app-dir/CWD
 * precedence). Returns the resolved path or empty.
 */
std::wstring resolveOnPathW(const std::wstring& exe);

/** Narrow wrappers for tests and ASCII-only callers. */
std::string quoteProcessArg(const std::string& arg);
bool runProcess(const std::string& exe, const std::vector<std::string>& args, uint32_t timeoutMs);
std::string resolveOnPath(const std::string& exe);

#endif // _WIN32

} // namespace byteback
