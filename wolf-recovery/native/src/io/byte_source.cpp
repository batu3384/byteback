#include "io/byte_source.h"

#include <fstream>
#include <algorithm>
#include <cstring>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace wolf {

namespace {

class FileByteSource final : public ByteSource {
public:
    explicit FileByteSource(const std::string& path) {
        file_.open(path, std::ios::binary);
        if (!file_.is_open()) {
            err_ = "could not open file";
            return;
        }
        file_.seekg(0, std::ios::end);
        size_ = static_cast<uint64_t>(file_.tellg());
        file_.seekg(0, std::ios::beg);
    }

    bool read(uint64_t offset, uint8_t* buf, size_t len) override {
        if (!file_.is_open() || !buf) return false;
        if (offset + len > size_) {
            err_ = "read past end";
            return false;
        }
        file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        file_.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(len));
        if (!file_) {
            err_ = "file read failed";
            return false;
        }
        return true;
    }

    uint64_t size() const override { return size_; }
    std::string lastError() const override { return err_; }

private:
    std::ifstream file_;
    uint64_t size_ = 0;
    std::string err_;
};

#ifdef _WIN32
class HttpRangeByteSource final : public ByteSource {
public:
    explicit HttpRangeByteSource(const std::string& url) {
        parseUrl(url);
        if (err_.empty()) probeSize();
    }

    ~HttpRangeByteSource() override {
        if (hSession_) WinHttpCloseHandle(hSession_);
        if (hConnect_) WinHttpCloseHandle(hConnect_);
    }

    bool read(uint64_t offset, uint8_t* buf, size_t len) override {
        if (!buf || len == 0) return true;
        if (offset + len > size_) {
            err_ = "http read past end";
            return false;
        }
        if (!hSession_) return false;

        HINTERNET hRequest = WinHttpOpenRequest(hConnect_, L"GET", path_.c_str(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                useTls_ ? WINHTTP_FLAG_SECURE : 0);
        if (!hRequest) {
            err_ = "WinHttpOpenRequest failed";
            return false;
        }

        wchar_t rangeHdr[128];
        swprintf_s(rangeHdr, L"Range: bytes=%llu-%llu",
                   static_cast<unsigned long long>(offset),
                   static_cast<unsigned long long>(offset + len - 1));
        WinHttpAddRequestHeaders(hRequest, rangeHdr, static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

        BOOL ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (!ok || !WinHttpReceiveResponse(hRequest, nullptr)) {
            err_ = "http range request failed";
            WinHttpCloseHandle(hRequest);
            return false;
        }

        size_t written = 0;
        while (written < len) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(len - written, avail));
            DWORD got = 0;
            if (!WinHttpReadData(hRequest, buf + written, chunk, &got) || got == 0) break;
            written += got;
        }
        WinHttpCloseHandle(hRequest);
        if (written != len) {
            err_ = "short http range read";
            return false;
        }
        return true;
    }

    uint64_t size() const override { return size_; }
    std::string lastError() const override { return err_; }

private:
    void parseUrl(const std::string& url) {
        std::wstring w(url.begin(), url.end());
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256] = {};
        wchar_t path[2048] = {};
        uc.lpszHostName = host;
        uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
        if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) {
            err_ = "invalid http url";
            return;
        }
        host_ = host;
        path_ = path;
        port_ = uc.nPort ? uc.nPort : (uc.nScheme == INTERNET_SCHEME_HTTPS ? 443 : 80);
        useTls_ = uc.nScheme == INTERNET_SCHEME_HTTPS;
        hSession_ = WinHttpOpen(L"WolfRecovery/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession_) {
            err_ = "WinHttpOpen failed";
            return;
        }
        hConnect_ = WinHttpConnect(hSession_, host_.c_str(), port_, 0);
        if (!hConnect_) err_ = "WinHttpConnect failed";
    }

    void probeSize() {
        if (!hConnect_) return;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect_, L"HEAD", path_.c_str(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                useTls_ ? WINHTTP_FLAG_SECURE : 0);
        if (!hRequest) {
            err_ = "http HEAD failed";
            return;
        }
        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(hRequest, nullptr)) {
            WinHttpCloseHandle(hRequest);
            err_ = "http HEAD request failed";
            return;
        }
        wchar_t lenBuf[64] = {};
        DWORD lenSize = sizeof(lenBuf);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                lenBuf, &lenSize, WINHTTP_NO_HEADER_INDEX)) {
            size_ = _wcstoui64(lenBuf, nullptr, 10);
        }
        WinHttpCloseHandle(hRequest);
        if (size_ == 0) err_ = "http content-length unknown";
    }

    HINTERNET hSession_ = nullptr;
    HINTERNET hConnect_ = nullptr;
    std::wstring host_;
    std::wstring path_;
    INTERNET_PORT port_ = 80;
    bool useTls_ = false;
    uint64_t size_ = 0;
    std::string err_;
};
#endif

} // namespace

namespace {

bool parseHttpHost(const std::string& url, std::string& hostOut) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return false;
    size_t start = scheme + 3;
    if (start >= url.size()) return false;
    if (url[start] == '[') {
        const size_t end = url.find(']', start);
        if (end == std::string::npos) return false;
        hostOut = url.substr(start + 1, end - start - 1);
        return !hostOut.empty();
    }
    const size_t end = url.find_first_of(":/", start);
    hostOut = (end == std::string::npos) ? url.substr(start) : url.substr(start, end - start);
    return !hostOut.empty();
}

bool ipv4Octets(const std::string& host, uint8_t o[4]) {
    int parts[4] = {};
    char tail = 0;
    if (std::sscanf(host.c_str(), "%d.%d.%d.%d%c", &parts[0], &parts[1], &parts[2], &parts[3], &tail) != 4) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (parts[i] < 0 || parts[i] > 255) return false;
        o[i] = static_cast<uint8_t>(parts[i]);
    }
    return true;
}

bool isBlockedHostLiteral(const std::string& host) {
    std::string h = host;
    std::transform(h.begin(), h.end(), h.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (h == "localhost" || h == "0.0.0.0" || h == "::1" || h == "::") return true;
    if (h.size() >= 6 && h.compare(h.size() - 6, 6, ".local") == 0) return true;

    uint8_t o[4] = {};
    if (ipv4Octets(h, o)) {
        if (o[0] == 127) return true;
        if (o[0] == 10) return true;
        if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) return true;
        if (o[0] == 192 && o[1] == 168) return true;
        if (o[0] == 169 && o[1] == 254) return true;
        if (o[0] == 0) return true;
        return false;
    }

    if (!h.empty() && h[0] == '[') {
        const std::string inner = h.substr(1, h.size() - 2);
        if (inner == "::1") return true;
        if (inner.rfind("fe80:", 0) == 0 || inner.rfind("fc", 0) == 0 || inner.rfind("fd", 0) == 0) {
            return true;
        }
    }
    if (h.rfind("fe80:", 0) == 0 || h.rfind("fc", 0) == 0 || h.rfind("fd", 0) == 0) return true;
    return false;
}

} // namespace

bool httpUrlHostAllowed(const std::string& url) {
    if (!isHttpUrl(url)) return false;
    std::string host;
    if (!parseHttpHost(url, host)) return false;
    return !isBlockedHostLiteral(host);
}

bool isHttpUrl(const std::string& s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

std::unique_ptr<ByteSource> openFileByteSource(const std::string& path, std::string& err) {
    auto src = std::make_unique<FileByteSource>(path);
    if (src->size() == 0 && src->lastError().empty()) {
        // empty file is valid
    }
    if (!src->lastError().empty()) {
        err = src->lastError();
        return nullptr;
    }
    return src;
}

std::unique_ptr<ByteSource> openHttpByteSource(const std::string& url, std::string& err) {
#ifndef _WIN32
    (void)url;
    err = "http range source requires Windows WinHTTP";
    return nullptr;
#else
    if (!isHttpUrl(url)) {
        err = "not an http(s) url";
        return nullptr;
    }
    if (!httpUrlHostAllowed(url)) {
        err = "http url host not allowed";
        return nullptr;
    }
    auto src = std::make_unique<HttpRangeByteSource>(url);
    if (!src->lastError().empty() || src->size() == 0) {
        err = src->lastError().empty() ? "http size probe failed" : src->lastError();
        return nullptr;
    }
    return src;
#endif
}

} // namespace wolf
