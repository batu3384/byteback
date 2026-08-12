#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace wolf {
    class DiskReader;
}

using FileRecordCallback = std::function<void(const std::string& name, uint64_t size, uint64_t cluster, const std::string& path, int status)>;

class FATParser {
public:
    FATParser();
    ~FATParser();

    void parse(wolf::DiskReader& reader, uint64_t partitionOffset, FileRecordCallback callback);

private:
    void parseFAT(wolf::DiskReader& reader, uint64_t partitionOffset, FileRecordCallback callback);
    void parseExFAT(wolf::DiskReader& reader, uint64_t partitionOffset, FileRecordCallback callback);
};
