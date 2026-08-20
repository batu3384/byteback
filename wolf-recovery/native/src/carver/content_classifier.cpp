#include "carver/content_classifier.h"
#include "math/entropy_calculator.h"

#include <cctype>
#include <algorithm>

namespace wolf {

namespace {

bool mostlyPrintable(const uint8_t* data, size_t n) {
    if (n == 0) return false;
    size_t printable = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = data[i];
        if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c < 0x7f)) ++printable;
    }
    return printable * 100 / n >= 85;
}

} // namespace

std::string refineCarveCategory(const uint8_t* data, size_t size, const std::string& extension,
                                const std::string& currentCategory) {
    if (!currentCategory.empty() && currentCategory != "Unknown" && currentCategory != "Other") {
        return currentCategory;
    }
    if (!data || size == 0) return currentCategory.empty() ? "Unknown" : currentCategory;

    const size_t sample = std::min<size_t>(size, 4096);
    const double ent = math::calculateEntropy(data, sample);

    if (ent >= 7.6) return "Encrypted";

    std::string ext = extension;
    if (!ext.empty() && ext[0] == '.') ext.erase(ext.begin());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "bmp" ||
        ext == "webp") {
        return "Image";
    }
    if (ext == "mp4" || ext == "mov" || ext == "mkv" || ext == "avi" || ext == "ts") {
        return "Video";
    }
    if (ext == "mp3" || ext == "wav" || ext == "flac" || ext == "ogg") return "Audio";
    if (ext == "pdf" || ext == "doc" || ext == "docx" || ext == "xls" || ext == "xlsx") {
        return "Document";
    }
    if (ext == "zip" || ext == "rar" || ext == "7z") return "Archive";

    if (ent <= 4.2 && mostlyPrintable(data, sample)) return "Text";

    return currentCategory.empty() ? "Unknown" : currentCategory;
}

} // namespace wolf
