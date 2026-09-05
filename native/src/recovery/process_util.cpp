#include "recovery/process_util.h"

#ifdef _WIN32

#include <windows.h>

#include <sstream>
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

std::wstring quoteProcessArgW(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out;
    out.reserve(arg.size() + 2);
    out.push_back(L'"');
    size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            // Backslashes before a quote double up, plus one escaping the quote.
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    // Backslashes before the closing quote are doubled too.
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

bool runProcessW(const std::wstring& exe, const std::vector<std::wstring>& args, uint32_t timeoutMs) {
    std::wstring cmdline = quoteProcessArgW(exe);
    for (const std::wstring& a : args) {
        cmdline.push_back(L' ');
        cmdline += quoteProcessArgW(a);
    }
    if (cmdline.empty()) return false;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    // CreateProcessW may write a null terminator into the buffer: mutable copy.
    std::vector<wchar_t> cmdBuf(cmdline.begin(), cmdline.end());
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

namespace {

bool endsWithExe(const std::wstring& p) {
    return p.size() >= 4 &&
           (p.compare(p.size() - 4, 4, L".exe") == 0 || p.compare(p.size() - 4, 4, L".EXE") == 0);
}

bool isFile(const std::wstring& p) {
    const DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace

std::wstring resolveOnPathW(const std::wstring& exe) {
    if (exe.empty()) return {};
    if (exe.find(L'\\') != std::wstring::npos || exe.find(L'/') != std::wstring::npos) {
        // Explicit path: must be an .exe file as-is (F3: no .bat/.cmd, no
        // extension guessing — the caller names exactly what should run).
        return (endsWithExe(exe) && isFile(exe)) ? exe : std::wstring();
    }
    // Bare name: PATH entries only, .exe appended — never the application
    // directory or CWD first (F2: SearchPathW order enabled planting).
    const wchar_t* pathEnv = _wgetenv(L"PATH");
    if (!pathEnv) return {};
    std::wstringstream ss(pathEnv);
    std::wstring dir;
    while (std::getline(ss, dir, L';')) {
        // Strip surrounding quotes; skip empty and non-absolute entries.
        // The '.' prefix check is not enough: a bare relative entry like
        // "plant" also resolves against the CWD and re-opens the planting
        // hole, so require drive-letter or UNC syntax.
        if (!dir.empty() && dir.front() == L'"') dir.erase(0, 1);
        if (!dir.empty() && dir.back() == L'"') dir.pop_back();
        if (dir.empty() || dir.front() == L'.') continue;
        const bool absolute = (dir.size() >= 2 && dir[1] == L':') ||
                              (dir.size() >= 2 && dir[0] == L'\\' && dir[1] == L'\\');
        if (!absolute) continue;
        std::wstring full = dir;
        if (full.back() != L'\\' && full.back() != L'/') full.push_back(L'\\');
        full += exe;
        if (!endsWithExe(exe)) full += L".exe";
        if (isFile(full)) return full;
    }
    return {};
}

std::string quoteProcessArg(const std::string& arg) {
    const std::wstring w = quoteProcessArgW(utf8ToWide(arg));
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len > 0 ? len : 0), '\0');
    if (len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    }
    return out;
}

bool runProcess(const std::string& exe, const std::vector<std::string>& args, uint32_t timeoutMs) {
    std::vector<std::wstring> wide;
    wide.reserve(args.size());
    for (const std::string& a : args) wide.push_back(utf8ToWide(a));
    return runProcessW(utf8ToWide(exe), wide, timeoutMs);
}

std::string resolveOnPath(const std::string& exe) {
    const std::wstring w = resolveOnPathW(utf8ToWide(exe));
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len > 0 ? len : 0), '\0');
    if (len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    }
    return out;
}

} // namespace byteback

#endif // _WIN32
