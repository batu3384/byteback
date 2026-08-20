#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>

namespace wolf {

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

} // namespace wolf
