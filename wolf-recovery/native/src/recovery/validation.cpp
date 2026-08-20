#include "recovery/validation.h"
#include "fs/refs_integrity.h"
#include "carver/file_validators.h"
#include "carver/structural_parsers.h"
#include <algorithm>
#include <cctype>
#include <fstream>

namespace wolf {

namespace {

int dispatchValidator(const std::string& ext, const uint8_t* data, size_t size) {
    using namespace carver;
    if (ext == "jpg" || ext == "jpeg") return validateJpeg(data, size);
    if (ext == "png") return validatePng(data, size);
    if (ext == "zip" || ext == "docx" || ext == "xlsx" || ext == "pptx" ||
        ext == "odt" || ext == "ods" || ext == "odp" || ext == "epub" ||
        ext == "jar") return validateZip(data, size);
    if (ext == "pdf") return validatePdf(data, size);
    if (ext == "gz" || ext == "gzip" || ext == "tgz") return validateGzip(data, size);
    if (ext == "riff") return validateRiff(data, size);
    if (ext == "ts") return validateMpegTs(data, size);
    if (ext == "sqlite" || ext == "db") return validateSqlite(data, size);
    if (ext == "mp4" || ext == "mov" || ext == "m4v" || ext == "m4a" ||
        ext == "qt" || ext == "3gp") return validateMp4(data, size);
    return 90;
}

bool isCarveSource(const std::string& source) {
    return source == "carver" || source == "carver_bgc";
}

} // namespace

std::string extensionFromRecord(const FileRecord& record) {
    if (!record.extension.empty()) {
        std::string ext = record.extension;
        if (!ext.empty() && ext[0] == '.') ext.erase(ext.begin());
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!ext.empty()) return ext;
    }
    auto dot = record.name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= record.name.size()) return {};
    std::string ext = record.name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

int validateCarvedBuffer(const std::string& ext, const uint8_t* data, size_t size) {
    if (!data || size == 0) return 0;
    return dispatchValidator(ext, data, size);
}

void applyPostRecoveryValidation(RecoveryResult& result, const FileRecord& record) {
    if (!result.success || result.destPath.empty()) return;

    if (record.source == "refs" && record.integrityChecksum != 0 && !record.residentData.empty()) {
        const uint64_t got = refsCrc64Ecma(record.residentData.data(), record.residentData.size());
        if (got != record.integrityChecksum) {
            result.validationScore = 15;
            result.validationError = "ReFS integrity checksum mismatch";
            return;
        }
        result.validationScore = 90;
        return;
    }

    if (!isCarveSource(record.source)) return;

    std::ifstream in(result.destPath, std::ios::binary);
    if (!in) {
        result.validationScore = 0;
        result.validationError = "could not reopen recovered file";
        return;
    }

    constexpr size_t kMax = 1 << 20;
    std::vector<uint8_t> buf(kMax);
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(kMax));
    size_t n = static_cast<size_t>(in.gcount());
    if (n == 0) {
        result.validationScore = 0;
        result.validationError = "empty recovered file";
        return;
    }

    const std::string ext = extensionFromRecord(record);
    result.validationScore = validateCarvedBuffer(ext, buf.data(), n);
    if (result.validationScore < 60) {
        result.validationError = "structure validation failed";
    }
}

} // namespace wolf
