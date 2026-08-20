#include "recovery/preview_reader.h"
#include "byteback_memory.h"
#include <algorithm>
#include <cstring>

namespace byteback {

namespace {

std::string classifyPreviewKind(const FileRecord& record, const uint8_t* data, size_t size) {
    const std::string ext = [&]() {
        auto dot = record.name.find_last_of('.');
        if (dot == std::string::npos) return std::string{};
        std::string e = record.name.substr(dot + 1);
        std::transform(e.begin(), e.end(), e.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return e;
    }();

    if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "webp" ||
        ext == "bmp" || ext == "ico") {
        return "image";
    }
    if (ext == "pdf") return "pdf";
    if (ext == "txt" || ext == "log" || ext == "csv" || ext == "json" || ext == "xml" ||
        ext == "md" || ext == "html" || ext == "htm") {
        return "text";
    }
    if (size >= 4) {
        if (data[0] == 0xFF && data[1] == 0xD8) return "image";
        if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') return "image";
        if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F') return "image";
        if (data[0] == '%' && data[1] == 'P' && data[2] == 'D' && data[3] == 'F') return "pdf";
    }
    if (size > 0) {
        size_t printable = 0;
        size_t sample = std::min(size, size_t{512});
        for (size_t i = 0; i < sample; ++i) {
            unsigned char c = data[i];
            if (c == 9 || c == 10 || c == 13 || (c >= 32 && c < 127)) ++printable;
        }
        if (printable * 100 / sample >= 85) return "text";
    }
    return "binary";
}

} // namespace

bool readRecordPrefix(DiskReader& reader, const FileRecord& record, std::vector<uint8_t>& out,
                      size_t maxBytes) {
    out.clear();
    if (maxBytes == 0) return false;

    if (!record.residentData.empty()) {
        size_t n = std::min(maxBytes, record.residentData.size());
        if (record.sizeBytes > 0) n = std::min(n, static_cast<size_t>(record.sizeBytes));
        out.assign(record.residentData.begin(), record.residentData.begin() + n);
        return true;
    }

    if (!reader.isOpen() && !reader.hasRaidBackend()) return false;

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    uint64_t want = maxBytes;
    if (record.sizeBytes > 0) want = std::min(want, record.sizeBytes);

    auto appendFromRuns = [&](const std::vector<FileRecord::DataRun>& runs) -> bool {
        uint64_t filled = 0;
        for (const auto& run : runs) {
            if (filled >= want) break;
            if (run.startSector == UINT64_MAX) {
                uint64_t gap = static_cast<uint64_t>(run.sectorCount) * sectorSize;
                uint64_t take = std::min(gap, want - filled);
                out.insert(out.end(), take, 0);
                filled += take;
                continue;
            }
            uint64_t runBytes = static_cast<uint64_t>(run.sectorCount) * sectorSize;
            uint64_t take = std::min(runBytes, want - filled);
            if (take == 0) continue;
            uint32_t readSize = static_cast<uint32_t>(((take + sectorSize - 1) / sectorSize) * sectorSize);
            std::vector<uint8_t> buf(readSize);
            auto res = reader.readSectors(run.startSector * sectorSize, readSize, buf.data());
            if (!res.success || res.bytesRead == 0) return false;
            size_t copy = std::min(static_cast<size_t>(take), static_cast<size_t>(res.bytesRead));
            out.insert(out.end(), buf.begin(), buf.begin() + copy);
            filled += copy;
        }
        return filled > 0;
    };

    if (!record.runs.empty()) {
        return appendFromRuns(record.runs);
    }

    if (record.startSector > 0 || record.sizeBytes > 0) {
        uint64_t start = record.startSector * sectorSize;
        uint32_t readSize = static_cast<uint32_t>(((want + sectorSize - 1) / sectorSize) * sectorSize);
        std::vector<uint8_t> buf(readSize);
        auto res = reader.readSectors(start, readSize, buf.data());
        if (!res.success || res.bytesRead == 0) return false;
        size_t copy = std::min(static_cast<size_t>(want), static_cast<size_t>(res.bytesRead));
        out.assign(buf.begin(), buf.begin() + copy);
        return true;
    }

    return false;
}

FilePreviewResult readFilePreview(DiskReader& reader, const FileRecord& record) {
    FilePreviewResult out;
    if (!readRecordPrefix(reader, record, out.data, kPreviewMaxBytes)) {
        out.error = "could not read preview bytes";
        return out;
    }
    out.success = true;
    out.kind = classifyPreviewKind(record, out.data.data(), out.data.size());
    return out;
}

} // namespace byteback
