#include "recovery/validation.h"
#include "scan/discovery_sources.h"
#include "fs/refs_integrity.h"
#include "carver/file_validators.h"
#include "carver/structural_parsers.h"
#include "byteback_io.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace byteback {

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
    if (ext == "bmp") return validateBmp(data, size);
    if (ext == "ts") return validateMpegTs(data, size);
    if (ext == "sqlite" || ext == "db") return validateSqlite(data, size);
    if (ext == "mp4" || ext == "mov" || ext == "m4v" || ext == "m4a" ||
        ext == "qt" || ext == "3gp") return validateMp4(data, size);
    return -1; // no structural validator — do not fail on unknown types
}

bool isCarveSource(const std::string& source) {
    return source == "carver" || source == "carver_bgc";
}

bool isMetadataRecoverSource(const std::string& source) {
    if (source.empty() || isCarveSource(source)) return false;
    if (isDiscoverySourceName(source)) return false;
    return true;
}

bool hasStructuralValidator(const std::string& ext) {
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "zip" ||
           ext == "docx" || ext == "xlsx" || ext == "pptx" || ext == "odt" ||
           ext == "ods" || ext == "odp" || ext == "epub" || ext == "jar" ||
           ext == "pdf" || ext == "gz" || ext == "gzip" || ext == "tgz" ||
           ext == "riff" || ext == "bmp" || ext == "ts" || ext == "sqlite" ||
           ext == "db" || ext == "mp4" || ext == "mov" || ext == "m4v" ||
           ext == "m4a" || ext == "qt" || ext == "3gp";
}

bool needsTailSample(const std::string& ext) {
    return ext == "zip" || ext == "docx" || ext == "xlsx" || ext == "pptx" ||
           ext == "odt" || ext == "ods" || ext == "odp" || ext == "epub" ||
           ext == "jar" || ext == "pdf";
}

void checkSizeConsistency(RecoveryResult& result, const FileRecord& record) {
    if (record.sizeBytes == 0 || result.bytesRecovered == 0) return;
    if (result.bytesRecovered >= record.sizeBytes) return;

    const uint64_t pct = (result.bytesRecovered * 100) / record.sizeBytes;
    const std::string partial = "partial recovery: " + std::to_string(result.bytesRecovered) +
                                " / " + std::to_string(record.sizeBytes) + " bytes";
    if (result.validationError.empty()) result.validationError = partial;
    else result.validationError += "; " + partial;
    if (result.validationScore < 0 || static_cast<uint64_t>(result.validationScore) > pct) {
        result.validationScore = static_cast<int>(pct);
    }

    // Deleted metadata hits with truncated payload must not report success.
    if (record.status == 0 && isMetadataRecoverSource(record.source) && pct < 90) {
        result.success = false;
        if (result.error.empty()) result.error = partial;
    }
}

void validateRecoveredStructure(RecoveryResult& result, const FileRecord& record) {
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
        result.success = false;
        return;
    }

    const std::string ext = extensionFromRecord(record);
    if (!hasStructuralValidator(ext)) return;

    // ZIP EOCD / PDF %%EOF live at the file end. First-1-MiB-only used to
    // fail intact archives larger than the head window.
    if (needsTailSample(ext)) {
        in.clear();
        in.seekg(0, std::ios::end);
        const std::streamoff endPos = in.tellg();
        if (endPos > static_cast<std::streamoff>(n)) {
            constexpr std::streamoff kTail = 64 * 1024;
            const std::streamoff tailOff = endPos > kTail ? endPos - kTail : 0;
            in.clear();
            if (in.seekg(tailOff)) {
                const size_t tailCap = static_cast<size_t>(endPos - tailOff);
                std::vector<uint8_t> tail(tailCap);
                in.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tailCap));
                const size_t tn = static_cast<size_t>(in.gcount());
                buf.resize(n + tn);
                if (tn > 0) std::memcpy(buf.data() + n, tail.data(), tn);
                n += tn;
            }
        }
    }

    const int score = validateCarvedBuffer(ext, buf.data(), n);
    result.validationScore = score;
    if (score >= 0 && score < 60) {
        result.validationError = result.validationError.empty()
                                     ? "structure validation failed"
                                     : result.validationError + "; structure validation failed";
        result.success = false;
    }
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
    const int score = dispatchValidator(ext, data, size);
    return score < 0 ? 0 : score;
}

void applyPostRecoveryValidation(RecoveryResult& result, const FileRecord& record) {
    if (!result.success || result.destPath.empty()) return;

    checkSizeConsistency(result, record);

    if (record.source == "refs" && record.integrityChecksum != 0 && !record.residentData.empty()) {
        const uint64_t got = refsCrc64Ecma(record.residentData.data(), record.residentData.size());
        if (got != record.integrityChecksum) {
            result.validationScore = 15;
            result.validationError = "ReFS integrity checksum mismatch";
            result.success = false;
            return;
        }
        result.validationScore = 90;
        return;
    }

    if (isCarveSource(record.source) || isMetadataRecoverSource(record.source)) {
        validateRecoveredStructure(result, record);
    }
}

} // namespace byteback
