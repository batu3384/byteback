#include "recovery/process_util.h"

#ifdef _WIN32

#include <windows.h>
#include <shellapi.h>

#include <vector>

namespace byteback {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len > 0 ? len : 0), L'\0');
    if (len > 0) {
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    }
    return out;
}

std::string quoteProcessArg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;
    std::string out;
    out.reserve(arg.size() + 2);
    out.push_back('"');
    size_t backslashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            // Backslashes before a quote double up, plus one escaping the quote.
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    // Backslashes before the closing quote are doubled too.
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

bool runProcess(const std::string& exe, const std::vector<std::string>& args, uint32_t timeoutMs) {
    std::string cmdline = quoteProcessArg(exe);
    for (const std::string& a : args) {
        cmdline.push_back(' ');
        cmdline += quoteProcessArg(a);
    }
    const std::wstring wcmd = utf8ToWide(cmdline);
    if (wcmd.empty()) return false;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    // CreateProcessW may write a null terminator into the buffer: mutable copy.
    std::vector<wchar_t> cmdBuf(wcmd.begin(), wcmd.end());
    cmdBuf.push_back(L'\0');

    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);

    bool success = false;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) {
        DWORD code = 1;
        if (GetExitCodeProcess(pi.hProcess, &code)) success = (code == 0);
    } else {
        // Overran the budget: kill and reap so no zombie handle remains.
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    CloseHandle(pi.hProcess);
    return success;
}

std::string resolveOnPath(const std::string& exe) {
    if (exe.empty()) return {};
    if (exe.find('/') != std::string::npos || exe.find('\\') != std::string::npos) {
        const DWORD attrs = GetFileAttributesW(utf8ToWide(exe).c_str());
        return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) ? exe : std::string();
    }
    std::wstring found(MAX_PATH, L'\0');
    wchar_t* filePart = nullptr;
    const DWORD n = SearchPathW(nullptr, utf8ToWide(exe).c_str(), L".exe",
                                static_cast<DWORD>(found.size()), found.data(), &filePart);
    if (n == 0 || n >= found.size()) return {};
    found.resize(n);
    // Back to UTF-8 so callers can log/handle it like the rest of the code.
    const int len = WideCharToMultiByte(CP_UTF8, 0, found.c_str(), static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len > 0 ? len : 0), '\0');
    if (len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, found.c_str(), static_cast<int>(n), out.data(), len, nullptr, nullptr);
    }
    return out;
}

} // namespace byteback

#endif // _WIN32
