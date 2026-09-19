#include "io/byte_source.h"
#include "recovery/path_util.h"

#include <fstream>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace byteback {

// Range-read status gate: 206 proves the server honored Range. A 200 carries
// the object from byte 0, which equals the requested bytes only when the
// request spans the whole resource.
bool httpRangeReadStatusOk(unsigned status, uint64_t offset, uint64_t len, uint64_t totalSize) {
    if (status == 206) return true;
    return status == 200 && offset == 0 && len == totalSize;
}

bool httpProbeStatusOk(unsigned status) {
    return status == 200 || status == 206;
}

bool httpProbeNeedsRangeGetFallback(unsigned status) {
    return status == 405 || status == 501;
}

bool httpParseContentRangeTotal(const std::string& header, uint64_t& total) {
    total = 0;
    const auto slash = header.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= header.size()) return false;
    if (header[slash + 1] == '*') return false;
    char* end = nullptr;
    total = std::strtoull(header.c_str() + slash + 1, &end, 10);
    return total > 0 && end != header.c_str() + slash + 1;
}

namespace {

class FileByteSource final : public ByteSource {
public:
    explicit FileByteSource(const std::string& path) {
        file_.open(utf8Path(path), std::ios::binary);
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

bool parseSplitNumericExt(const std::string& path, std::string& stem, int& n) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 4 != path.size()) return false;
    for (size_t i = 1; i <= 3; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(path[dot + i]))) return false;
    }
    n = (path[dot + 1] - '0') * 100 + (path[dot + 2] - '0') * 10 + (path[dot + 3] - '0');
    stem = path.substr(0, dot);
    return true;
}

std::string splitSegmentPath(const std::string& stem, int n) {
    char ext[8];
    std::snprintf(ext, sizeof(ext), ".%03d", n);
    return stem + ext;
}

std::vector<std::string> collectSplitRawSegments(const std::string& first) {
    std::string stem;
    int start = 0;
    if (!parseSplitNumericExt(first, stem, start) || (start != 0 && start != 1)) return {first};
    std::vector<std::string> out;
    out.reserve(8);
    for (int i = start; i < start + 256; ++i) {
        const std::string p = splitSegmentPath(stem, i);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(utf8Path(p), ec) || ec) break;
        out.push_back(p);
    }
    if (out.size() < 2) return {first};
    return out;
}

class SplitByteSource final : public ByteSource {
public:
    explicit SplitByteSource(const std::vector<std::string>& paths) {
        uint64_t base = 0;
        for (const auto& p : paths) {
            auto src = std::make_unique<FileByteSource>(p);
            if (!src->lastError().empty()) {
                err_ = src->lastError();
                parts_.clear();
                size_ = 0;
                return;
            }
            Part part;
            part.size = src->size();
            part.base = base;
            part.src = std::move(src);
            base += part.size;
            parts_.push_back(std::move(part));
        }
        size_ = base;
    }

    bool read(uint64_t offset, uint8_t* buf, size_t len) override {
        if (parts_.empty() || !buf) return false;
        if (len == 0) return true;
        if (offset + len > size_) {
            err_ = "read past end";
            return false;
        }
        size_t done = 0;
        while (done < len) {
            const uint64_t at = offset + done;
            const Part* part = nullptr;
            for (const auto& p : parts_) {
                if (at >= p.base && at < p.base + p.size) {
                    part = &p;
                    break;
                }
            }
            if (!part || !part->src) return false;
            const uint64_t local = at - part->base;
            const size_t n = static_cast<size_t>(
                std::min<uint64_t>(len - done, part->size - local));
            if (!part->src->read(local, buf + done, n)) {
                err_ = part->src->lastError();
                return false;
            }
            done += n;
        }
        return true;
    }

    uint64_t size() const override { return size_; }
    std::string lastError() const override { return err_; }

private:
    struct Part {
        std::unique_ptr<FileByteSource> src;
        uint64_t size = 0;
        uint64_t base = 0;
    };
    std::vector<Part> parts_;
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

        // One retry: a stalled/flaky link kills only this range, not the job.
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (readAttempt(offset, buf, len)) {
                err_.clear();
                return true;
            }
        }
        return false;
    }

    uint64_t size() const override { return size_; }
    std::string lastError() const override { return err_; }

private:
    bool readAttempt(uint64_t offset, uint8_t* buf, size_t len) {
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

        // A 200 to a ranged GET means the server ignored Range and sent the
        // object from byte 0 — data would be silently wrong at offset > 0.
        DWORD status = 0, statusSize = sizeof(status);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX);
        if (!httpRangeReadStatusOk(status, offset, len, size_)) {
            err_ = "http range not honored (status " + std::to_string(status) + ")";
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
        hSession_ = WinHttpOpen(L"Byteback/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession_) {
            err_ = "WinHttpOpen failed";
            return;
        }
        // Bound every phase so a stalled connection errors instead of hanging
        // on WinHTTP's generous defaults.
        WinHttpSetTimeouts(hSession_, 15000, 20000, 20000, 30000);
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
        // Never trust a Content-Length that describes an error/redirect page.
        DWORD status = 0, statusSize = sizeof(status);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                WINHTTP_NO_HEADER_INDEX) &&
            !httpProbeStatusOk(status)) {
            const unsigned headStatus = status;
            WinHttpCloseHandle(hRequest);
            if (httpProbeNeedsRangeGetFallback(headStatus)) {
                probeSizeViaRangeGet();
                return;
            }
            err_ = "http HEAD status " + std::to_string(headStatus);
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

    void probeSizeViaRangeGet() {
        if (!hConnect_) return;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect_, L"GET", path_.c_str(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                useTls_ ? WINHTTP_FLAG_SECURE : 0);
        if (!hRequest) {
            err_ = "http HEAD-less GET failed";
            return;
        }
        WinHttpAddRequestHeaders(hRequest, L"Range: bytes=0-0", static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(hRequest, nullptr)) {
            WinHttpCloseHandle(hRequest);
            err_ = "http HEAD-less range request failed";
            return;
        }
        DWORD status = 0, statusSize = sizeof(status);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX);
        if (status != 206) {
            WinHttpCloseHandle(hRequest);
            err_ = "http HEAD-less range not honored (status " + std::to_string(status) + ")";
            return;
        }
        wchar_t crBuf[128] = {};
        DWORD crSize = sizeof(crBuf);
        if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX,
                                 crBuf, &crSize, WINHTTP_NO_HEADER_INDEX)) {
            WinHttpCloseHandle(hRequest);
            err_ = "http HEAD-less Content-Range missing";
            return;
        }
        std::string cr;
        for (const wchar_t* p = crBuf; *p; ++p) cr.push_back(static_cast<char>(*p));
        uint64_t total = 0;
        if (!httpParseContentRangeTotal(cr, total)) {
            WinHttpCloseHandle(hRequest);
            err_ = "http HEAD-less Content-Range unusable";
            return;
        }
        size_ = total;
        WinHttpCloseHandle(hRequest);
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
    const auto segs = collectSplitRawSegments(path);
    std::unique_ptr<ByteSource> src;
    if (segs.size() >= 2) {
        src = std::make_unique<SplitByteSource>(segs);
    } else {
        src = std::make_unique<FileByteSource>(path);
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

} // namespace byteback
