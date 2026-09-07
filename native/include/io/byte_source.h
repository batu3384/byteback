#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>

namespace byteback {

// Minimal random-access byte source for image backends (local file, HTTP Range).
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual bool read(uint64_t offset, uint8_t* buf, size_t len) = 0;
    virtual uint64_t size() const = 0;
    virtual std::string lastError() const { return {}; }
};

std::unique_ptr<ByteSource> openFileByteSource(const std::string& path, std::string& err);
std::unique_ptr<ByteSource> openHttpByteSource(const std::string& url, std::string& err);

bool isHttpUrl(const std::string& s);

/** Returns false for localhost, link-local, and RFC1918 targets (SSRF guard). */
bool httpUrlHostAllowed(const std::string& url);

/** True if the HTTP status of a range GET may be treated as range data.
 *  Requires 206 unless the request spans the whole resource, in which case a
 *  200 (body starting at offset 0) carries exactly the requested bytes. */
bool httpRangeReadStatusOk(unsigned status, uint64_t offset, uint64_t len, uint64_t totalSize);

/** True if the HTTP status of a size probe is a success whose Content-Length
 *  describes the real object (not an error/redirect page body). */
bool httpProbeStatusOk(unsigned status);

} // namespace byteback
