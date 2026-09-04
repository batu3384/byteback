#pragma once

// CA-039: shell-free child process execution. The old preview path built
// cmd.exe command lines by quote concatenation and interpolated the
// BYTEBACK_FFMPEG env value unvalidated — a crafted value could break out of
// the quoting. CreateProcessW takes the command line directly: no shell, and
// every argument goes through argv-rule quoting.

#include <cstdint>
#include <string>
#include <vector>

namespace byteback {

#ifdef _WIN32

/** UTF-8 -> UTF-16. */
std::wstring utf8ToWide(const std::string& s);

/**
 * Quote one argument per Windows argv parsing rules (double embedded quotes,
 * double backslash runs that precede a quote). Bare arguments stay bare.
 */
std::string quoteProcessArg(const std::string& arg);

/**
 * Run `exe` with `args` (no shell). Returns true iff the process launched,
 * exited within timeoutMs and its exit code was 0. On timeout the child is
 * terminated. Hidden window: no console flash for GUI-less parents.
 */
bool runProcess(const std::string& exe, const std::vector<std::string>& args, uint32_t timeoutMs);

/**
 * Resolve an executable: absolute/relative paths must exist; a bare name is
 * searched on PATH via SearchPathW (replaces the old `where` shell probe).
 * Returns the resolved absolute path, or empty when not found.
 */
std::string resolveOnPath(const std::string& exe);

#endif // _WIN32

} // namespace byteback
