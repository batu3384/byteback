#include "io/byte_source.h"

#include <fstream>
#include <algorithm>
#include <cstring>

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
    auto src = std::make_unique<HttpRangeByteSource>(url);
    if (!src->lastError().empty() || src->size() == 0) {
        err = src->lastError().empty() ? "http size probe failed" : src->lastError();
        return nullptr;
    }
    return src;
#endif
}

} // namespace wolf
