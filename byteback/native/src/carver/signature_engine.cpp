#include "byteback_carver.h"
#include "byteback_memory.h"
#include "carver/file_validators.h"
#include "carver/structural_parsers.h"
#include "carver/content_classifier.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <future>
#include <algorithm>
#include <regex>
#include <functional>
#include <mutex>
#include "util/thread_pool.h"

namespace byteback {

namespace {
// Dispatch a carved buffer to the right Fast Object Validator based on the
// signature's extension. Returns the validator's [0,100] confidence, or a
// neutral 90 if no structural validator exists for this type (so unknown
// types keep their header/footer score instead of being penalised).
int dispatchValidator(const std::string& ext, const uint8_t* data, size_t size) {
    using namespace byteback::carver;
    if (ext == "jpg" || ext == "jpeg") return validateJpeg(data, size);
    if (ext == "png")                   return validatePng(data, size);
    if (ext == "zip" || ext == "docx" || ext == "xlsx" || ext == "pptx" ||
        ext == "odt" || ext == "ods" || ext == "odp" || ext == "epub" ||
        ext == "jar")                   return validateZip(data, size);
    if (ext == "pdf")                   return validatePdf(data, size);
    if (ext == "gz" || ext == "gzip" || ext == "tgz") return validateGzip(data, size);
    if (ext == "riff")                  return validateRiff(data, size);
    if (ext == "ts")                    return validateMpegTs(data, size);
    if (ext == "sqlite" || ext == "db") return carver::validateSqlite(data, size);
    if (ext == "mp4" || ext == "mov" || ext == "m4v" || ext == "m4a" ||
        ext == "qt" || ext == "3gp")   return carver::validateMp4(data, size);
    return 90; // no structural validator — trust the header/footer match
}

bool isZipFamilyExt(const std::string& ext) {
    return ext == "zip" || ext == "docx" || ext == "xlsx" || ext == "pptx" ||
           ext == "odt" || ext == "ods" || ext == "odp" || ext == "epub" || ext == "jar";
}

bool isMp4FamilyExt(const std::string& ext) {
    return ext == "mp4" || ext == "mov" || ext == "m4v" || ext == "m4a" ||
           ext == "qt" || ext == "3gp";
}

void applyStructuralRefinement(const std::string& ext, const uint8_t* data, size_t size,
                               uint64_t& actualSize, std::string& effExt, int& confidence) {
    carver::StructuralParseResult pr;
    if (isZipFamilyExt(ext)) pr = carver::parseZipFamily(data, size);
    else if (ext == "sqlite" || ext == "db") pr = carver::parseSqliteDb(data, size);
    else if (isMp4FamilyExt(ext)) pr = carver::parseMp4Mov(data, size);
    else return;

    if (!pr.valid) return;
    if (pr.size > 0 && pr.size <= actualSize) actualSize = pr.size;
    if (!pr.extension.empty()) effExt = pr.extension;
    if (pr.confidence > confidence) confidence = pr.confidence;
}
} // namespace

CarvingEngine::CarvingEngine() {}
CarvingEngine::~CarvingEngine() {}

std::vector<uint8_t> CarvingEngine::hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        if (i + 1 >= hex.length()) break;
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = (uint8_t)strtol(byteString.c_str(), nullptr, 16);
        bytes.push_back(byte);
    }
    return bytes;
}

namespace {

bool appendSignaturesFromJson(std::vector<FileSignature>& signatures,
                              const std::string& jsonPath,
                              const std::function<std::vector<uint8_t>(const std::string&)>& hexToBytes) {
    std::ifstream file(jsonPath);
    if (!file.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::regex sigRegex(
        "\\{\\s*\"format\"\\s*:\\s*\"([^\"]+)\",\\s*\"extension\"\\s*:\\s*\"([^\"]+)\",\\s*\"category\"\\s*:\\s*\"([^\"]+)\",\\s*\"header\"\\s*:\\s*\"([^\"]*)\",\\s*\"footer\"\\s*:\\s*\"([^\"]*)\",\\s*\"max_?[Ss]ize\"\\s*:\\s*(\\d+)\\s*\\}");

    size_t before = signatures.size();
    for (std::sregex_iterator i(content.begin(), content.end(), sigRegex), end; i != end; ++i) {
        std::smatch match = *i;
        FileSignature s;
        s.format = match[1].str();
        s.extension = match[2].str();
        s.category = match[3].str();
        s.header = hexToBytes(match[4].str());
        s.footer = hexToBytes(match[5].str());
        s.maxSize = std::stoull(match[6].str());
        s.id = static_cast<int>(signatures.size());
        signatures.push_back(s);
    }
    return signatures.size() > before;
}

void loadEmbeddedSignatures(std::vector<FileSignature>& signatures) {
    auto addSig = [&](const std::string& fmt, const std::string& ext, const std::string& cat,
                      const std::vector<uint8_t>& head, const std::vector<uint8_t>& foot, uint64_t maxS) {
        FileSignature s;
        s.id = static_cast<int>(signatures.size());
        s.format = fmt;
        s.extension = ext;
        s.category = cat;
        s.header = head;
        s.footer = foot;
        s.maxSize = maxS;
        signatures.push_back(s);
    };

    // ============================================================
    // Images (15 signatures)
    // ============================================================
    addSig("JPEG Image", ".jpg", "Image", {0xFF, 0xD8, 0xFF}, {0xFF, 0xD9}, 15 * 1024 * 1024);
    addSig("PNG Image", ".png", "Image", {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}, {0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82}, 30 * 1024 * 1024);
    addSig("GIF Image", ".gif", "Image", {0x47, 0x49, 0x46, 0x38}, {0x00, 0x3B}, 10 * 1024 * 1024);
    addSig("BMP Image", ".bmp", "Image", {0x42, 0x4D}, {}, 50 * 1024 * 1024);
    addSig("TIFF Image (LE)", ".tiff", "Image", {0x49, 0x49, 0x2A, 0x00}, {}, 100 * 1024 * 1024);
    addSig("TIFF Image (BE)", ".tiff", "Image", {0x4D, 0x4D, 0x00, 0x2A}, {}, 100 * 1024 * 1024);
    // CA-006: single RIFF-container signature. The subtype (WEBP/AVI/WAVE)
    // lives at bytes 8..11 and is resolved by validateRiff at emit time —
    // the old trio of same-magic signatures triple-reported every RIFF file.
    addSig("RIFF Container", ".riff", "Container", {0x52, 0x49, 0x46, 0x46}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("HEIC Image", ".heic", "Image", {0x00, 0x00, 0x00, 0x18, 0x66, 0x74, 0x79, 0x70, 0x68, 0x65, 0x69, 0x63}, {}, 30 * 1024 * 1024);
    addSig("PSD Photoshop", ".psd", "Image", {0x38, 0x42, 0x50, 0x53}, {}, 500 * 1024 * 1024);
    addSig("ICO Icon", ".ico", "Image", {0x00, 0x00, 0x01, 0x00}, {}, 1 * 1024 * 1024);
    addSig("CUR Cursor", ".cur", "Image", {0x00, 0x00, 0x02, 0x00}, {}, 1 * 1024 * 1024);
    addSig("TGA Image", ".tga", "Image", {}, {0x54, 0x52, 0x55, 0x45, 0x56, 0x49, 0x53, 0x49, 0x4F, 0x4E}, 50 * 1024 * 1024);
    addSig("SVG Image", ".svg", "Image", {0x3C, 0x73, 0x76, 0x67}, {}, 10 * 1024 * 1024);
    addSig("Adobe AI", ".ai", "Image", {0x25, 0x50, 0x44, 0x46}, {}, 200 * 1024 * 1024);
    addSig("Canon CR2 RAW", ".cr2", "Image", {0x49, 0x49, 0x2A, 0x00, 0x10, 0x00, 0x00, 0x00, 0x43, 0x52}, {}, 50 * 1024 * 1024);

    // ============================================================
    // Documents (12 signatures)
    // ============================================================
    addSig("PDF Document", ".pdf", "Document", {0x25, 0x50, 0x44, 0x46, 0x2D}, {0x25, 0x25, 0x45, 0x4F, 0x46}, 100 * 1024 * 1024);
    addSig("RTF Document", ".rtf", "Document", {0x7B, 0x5C, 0x72, 0x74, 0x66, 0x31}, {0x7D}, 20 * 1024 * 1024);
    addSig("MS Office (OLE2)", ".doc", "Document", {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1}, {}, 100 * 1024 * 1024);
    // CA-006: odt/epub/pages removed — they share the PK magic with
    // ZIP and produced duplicate records for the same bytes. One signature
    // (ZIP/DOCX/XLSX below) covers the whole container family; the format
    // distinction needs container-content inspection, which is out of scope
    // for header/footer carving.
    addSig("CHM Help File", ".chm", "Document", {0x49, 0x54, 0x53, 0x46, 0x03, 0x00, 0x00, 0x00}, {}, 30 * 1024 * 1024);
    addSig("XML Document", ".xml", "Document", {0x3C, 0x3F, 0x78, 0x6D, 0x6C}, {}, 50 * 1024 * 1024);
    addSig("HTML Page", ".html", "Document", {0x3C, 0x21, 0x44, 0x4F, 0x43, 0x54, 0x59, 0x50, 0x45}, {}, 10 * 1024 * 1024);
    addSig("LaTeX Document", ".tex", "Document", {0x5C, 0x64, 0x6F, 0x63, 0x75, 0x6D, 0x65, 0x6E, 0x74, 0x63, 0x6C, 0x61, 0x73, 0x73}, {}, 10 * 1024 * 1024);
    addSig("Markdown", ".md", "Document", {0x23, 0x20}, {}, 5 * 1024 * 1024);
    addSig("PostScript", ".ps", "Document", {0x25, 0x21, 0x50, 0x53}, {0x25, 0x25, 0x45, 0x4F, 0x46}, 50 * 1024 * 1024);
    addSig("DjVu Document", ".djvu", "Document", {0x41, 0x54, 0x26, 0x54, 0x46, 0x4F, 0x52, 0x4D}, {}, 100 * 1024 * 1024);

    // ============================================================
    // Video (14 signatures)
    // ============================================================
    addSig("MP4 Video", ".mp4", "Video", {0x00, 0x00, 0x00, 0x18, 0x66, 0x74, 0x79, 0x70}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("MP4 Video (Alt)", ".mp4", "Video", {0x00, 0x00, 0x00, 0x20, 0x66, 0x74, 0x79, 0x70}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("MP4 Video (Alt2)", ".mp4", "Video", {0x00, 0x00, 0x00, 0x1C, 0x66, 0x74, 0x79, 0x70}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("MKV Video", ".mkv", "Video", {0x1A, 0x45, 0xDF, 0xA3}, {}, 4ULL * 1024 * 1024 * 1024);
    addSig("MPEG Video", ".mpg", "Video", {0x00, 0x00, 0x01, 0xBA}, {0x00, 0x00, 0x01, 0xB9}, 2ULL * 1024 * 1024 * 1024);
    addSig("MOV Video", ".mov", "Video", {0x00, 0x00, 0x00, 0x14, 0x66, 0x74, 0x79, 0x70, 0x71, 0x74}, {}, 4ULL * 1024 * 1024 * 1024);
    addSig("FLV Video", ".flv", "Video", {0x46, 0x4C, 0x56, 0x01}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("WMV Video", ".wmv", "Video", {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("3GP Video", ".3gp", "Video", {0x00, 0x00, 0x00, 0x14, 0x66, 0x74, 0x79, 0x70, 0x33, 0x67, 0x70}, {}, 1ULL * 1024 * 1024 * 1024);
    addSig("WebM Video", ".webm", "Video", {0x1A, 0x45, 0xDF, 0xA3}, {}, 4ULL * 1024 * 1024 * 1024);
    addSig("MPEG-TS", ".ts", "Video", {0x47, 0x40, 0x00}, {}, 4ULL * 1024 * 1024 * 1024);
    addSig("M4V Video", ".m4v", "Video", {0x00, 0x00, 0x00, 0x18, 0x66, 0x74, 0x79, 0x70, 0x4D, 0x34, 0x56}, {}, 4ULL * 1024 * 1024 * 1024);
    addSig("SWF Flash", ".swf", "Video", {0x46, 0x57, 0x53}, {}, 100 * 1024 * 1024);

    // ============================================================
    // Audio (12 signatures)
    // ============================================================
    addSig("MP3 Audio", ".mp3", "Audio", {0x49, 0x44, 0x33}, {}, 20 * 1024 * 1024);
    addSig("MP3 Audio (no ID3)", ".mp3", "Audio", {0xFF, 0xFB}, {}, 20 * 1024 * 1024);
    addSig("FLAC Audio", ".flac", "Audio", {0x66, 0x4C, 0x61, 0x43}, {}, 100 * 1024 * 1024);
    addSig("OGG Audio", ".ogg", "Audio", {0x4F, 0x67, 0x67, 0x53}, {}, 100 * 1024 * 1024);
    addSig("AAC Audio", ".aac", "Audio", {0xFF, 0xF1}, {}, 20 * 1024 * 1024);
    addSig("WMA Audio", ".wma", "Audio", {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11}, {}, 50 * 1024 * 1024);
    addSig("MIDI Audio", ".mid", "Audio", {0x4D, 0x54, 0x68, 0x64}, {}, 5 * 1024 * 1024);
    addSig("AIFF Audio", ".aiff", "Audio", {0x46, 0x4F, 0x52, 0x4D}, {}, 100 * 1024 * 1024);
    addSig("M4A Audio", ".m4a", "Audio", {0x00, 0x00, 0x00, 0x20, 0x66, 0x74, 0x79, 0x70, 0x4D, 0x34, 0x41}, {}, 50 * 1024 * 1024);
    addSig("APE Audio", ".ape", "Audio", {0x4D, 0x41, 0x43, 0x20}, {}, 100 * 1024 * 1024);
    addSig("WavPack Audio", ".wv", "Audio", {0x77, 0x76, 0x70, 0x6B}, {}, 100 * 1024 * 1024);

    // ============================================================
    // Archives (12 signatures)
    // ============================================================
    addSig("ZIP/DOCX/XLSX", ".zip", "Archive", {0x50, 0x4B, 0x03, 0x04}, {0x50, 0x4B, 0x05, 0x06}, 500 * 1024 * 1024);
    addSig("RAR Archive", ".rar", "Archive", {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00}, {}, 500 * 1024 * 1024);
    addSig("RAR Archive v5", ".rar", "Archive", {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00}, {}, 500 * 1024 * 1024);
    addSig("7-Zip Archive", ".7z", "Archive", {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C}, {}, 500 * 1024 * 1024);
    addSig("GZIP Archive", ".gz", "Archive", {0x1F, 0x8B, 0x08}, {}, 100 * 1024 * 1024);
    addSig("BZIP2 Archive", ".bz2", "Archive", {0x42, 0x5A, 0x68}, {}, 100 * 1024 * 1024);
    addSig("XZ Archive", ".xz", "Archive", {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00}, {}, 500 * 1024 * 1024);
    addSig("TAR Archive", ".tar", "Archive", {0x75, 0x73, 0x74, 0x61, 0x72}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("ZSTD Archive", ".zst", "Archive", {0x28, 0xB5, 0x2F, 0xFD}, {}, 500 * 1024 * 1024);
    addSig("LZ4 Archive", ".lz4", "Archive", {0x04, 0x22, 0x4D, 0x18}, {}, 500 * 1024 * 1024);
    addSig("CAB Archive", ".cab", "Archive", {0x4D, 0x53, 0x43, 0x46}, {}, 500 * 1024 * 1024);
    addSig("MSI Installer", ".msi", "Archive", {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1}, {}, 500 * 1024 * 1024);

    // ============================================================
    // Databases & Email (8 signatures)
    // ============================================================
    addSig("SQLite Database", ".sqlite", "Database", {0x53, 0x51, 0x4C, 0x69, 0x74, 0x65, 0x20, 0x66, 0x6F, 0x72, 0x6D, 0x61, 0x74, 0x20, 0x33, 0x00}, {}, 1ULL * 1024 * 1024 * 1024);
    addSig("PST Email Data", ".pst", "Email", {0x21, 0x42, 0x44, 0x4E}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("OST Email Data", ".ost", "Email", {0x21, 0x42, 0x44, 0x4E}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("MBOX Email", ".mbox", "Email", {0x46, 0x72, 0x6F, 0x6D, 0x20}, {}, 1ULL * 1024 * 1024 * 1024);
    addSig("MS Access DB", ".mdb", "Database", {0x00, 0x01, 0x00, 0x00, 0x53, 0x74, 0x61, 0x6E, 0x64, 0x61, 0x72, 0x64, 0x20, 0x4A, 0x65, 0x74}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("MS Access 2007+", ".accdb", "Database", {0x00, 0x01, 0x00, 0x00, 0x53, 0x74, 0x61, 0x6E, 0x64, 0x61, 0x72, 0x64, 0x20, 0x41, 0x43, 0x45}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("Windows Registry", ".reg", "System", {0x72, 0x65, 0x67, 0x66}, {}, 100 * 1024 * 1024);
    addSig("Windows Shortcut", ".lnk", "System", {0x4C, 0x00, 0x00, 0x00, 0x01, 0x14, 0x02, 0x00}, {}, 1 * 1024 * 1024);

    // ============================================================
    // Executables, System & Forensics (9 signatures)
    // ============================================================
    addSig("NTFS MFT Record", ".mft", "System", {0x46, 0x49, 0x4C, 0x45, 0x30}, {}, 1024); // MFT FILE0 record is typically 1024 bytes
    addSig("Windows Executable", ".exe", "Executable", {0x4D, 0x5A}, {}, 100 * 1024 * 1024);
    addSig("ELF Executable", ".elf", "Executable", {0x7F, 0x45, 0x4C, 0x46}, {}, 100 * 1024 * 1024);
    addSig("Java Class", ".class", "Executable", {0xCA, 0xFE, 0xBA, 0xBE}, {}, 10 * 1024 * 1024);
    addSig("Mach-O Binary", ".macho", "Executable", {0xFE, 0xED, 0xFA, 0xCE}, {}, 100 * 1024 * 1024);
    addSig("Mach-O 64-bit", ".macho", "Executable", {0xFE, 0xED, 0xFA, 0xCF}, {}, 100 * 1024 * 1024);
    addSig("DEX Android", ".dex", "Executable", {0x64, 0x65, 0x78, 0x0A}, {}, 50 * 1024 * 1024);
    addSig("WASM Binary", ".wasm", "Executable", {0x00, 0x61, 0x73, 0x6D}, {}, 50 * 1024 * 1024);
    addSig("Windows DLL", ".dll", "Executable", {0x4D, 0x5A}, {}, 100 * 1024 * 1024);

    // ============================================================
    // Disk Images & Virtualization (8 signatures)
    // ============================================================
    addSig("ISO 9660 Image", ".iso", "DiskImage", {0x43, 0x44, 0x30, 0x30, 0x31}, {}, 8ULL * 1024 * 1024 * 1024);
    addSig("VHD Disk Image", ".vhd", "DiskImage", {0x63, 0x6F, 0x6E, 0x65, 0x63, 0x74, 0x69, 0x78}, {}, 100ULL * 1024 * 1024 * 1024);
    addSig("VMDK Disk Image", ".vmdk", "DiskImage", {0x4B, 0x44, 0x4D, 0x56}, {}, 100ULL * 1024 * 1024 * 1024);
    addSig("QCOW2 Disk Image", ".qcow2", "DiskImage", {0x51, 0x46, 0x49, 0xFB}, {}, 100ULL * 1024 * 1024 * 1024);
    addSig("VDI VirtualBox", ".vdi", "DiskImage", {0x3C, 0x3C, 0x3C, 0x20}, {}, 100ULL * 1024 * 1024 * 1024);
    addSig("DMG Apple", ".dmg", "DiskImage", {0x78, 0x01, 0x73, 0x0D, 0x62, 0x62, 0x60}, {}, 8ULL * 1024 * 1024 * 1024);
    addSig("LUKS Encrypted", ".luks", "Encrypted", {0x4C, 0x55, 0x4B, 0x53, 0xBA, 0xBE}, {}, 0);
    addSig("VeraCrypt Volume", ".hc", "Encrypted", {}, {}, 0);

    // ============================================================
    // Fonts (4 signatures)
    // ============================================================
    addSig("TrueType Font", ".ttf", "Font", {0x00, 0x01, 0x00, 0x00, 0x00}, {}, 10 * 1024 * 1024);
    addSig("OpenType Font", ".otf", "Font", {0x4F, 0x54, 0x54, 0x4F}, {}, 10 * 1024 * 1024);
    addSig("WOFF Font", ".woff", "Font", {0x77, 0x4F, 0x46, 0x46}, {}, 10 * 1024 * 1024);
    addSig("WOFF2 Font", ".woff2", "Font", {0x77, 0x4F, 0x46, 0x32}, {}, 10 * 1024 * 1024);

    // ============================================================
    // Misc (7 signatures)
    // ============================================================
    addSig("PCap Network", ".pcap", "Network", {0xD4, 0xC3, 0xB2, 0xA1}, {}, 1ULL * 1024 * 1024 * 1024);
    addSig("PCap-ng Network", ".pcapng", "Network", {0x0A, 0x0D, 0x0D, 0x0A}, {}, 1ULL * 1024 * 1024 * 1024);
    addSig("torrent File", ".torrent", "Misc", {0x64, 0x38, 0x3A, 0x61, 0x6E, 0x6E, 0x6F, 0x75, 0x6E, 0x63, 0x65}, {}, 5 * 1024 * 1024);
    addSig("iCalendar", ".ics", "Misc", {0x42, 0x45, 0x47, 0x49, 0x4E, 0x3A, 0x56, 0x43, 0x41, 0x4C}, {}, 1 * 1024 * 1024);
    addSig("vCard Contact", ".vcf", "Misc", {0x42, 0x45, 0x47, 0x49, 0x4E, 0x3A, 0x56, 0x43, 0x41, 0x52, 0x44}, {}, 1 * 1024 * 1024);
    addSig("GPX GPS Data", ".gpx", "Misc", {0x3C, 0x3F, 0x78, 0x6D, 0x6C}, {}, 10 * 1024 * 1024);
    addSig("KML Google Earth", ".kml", "Misc", {0x3C, 0x3F, 0x78, 0x6D, 0x6C}, {}, 10 * 1024 * 1024);

    // ============================================================
    // Extended signatures — high-value formats missing from the base set
    // ============================================================
    // Camera RAW (distinct magics; TIFF-based RAWs share II*\0 and are
    // already covered by the TIFF signature, so only non-TIFF RAWs here).
    addSig("Fuji RAF RAW", ".raf", "Image", {0x46, 0x55, 0x4A, 0x49, 0x46, 0x49, 0x4C, 0x4D, 0x43, 0x43, 0x44, 0x44, 0x2D, 0x52, 0x41, 0x57}, {}, 100 * 1024 * 1024);
    addSig("JPEG2000 JP2", ".jp2", "Image", {0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A}, {}, 50 * 1024 * 1024);
    addSig("Canon CR3 RAW", ".cr3", "Image", {0x66, 0x74, 0x79, 0x70, 0x63, 0x72, 0x78, 0x20}, {}, 100 * 1024 * 1024);
    // Documents
    addSig("Windows EDB", ".edb", "Database", {0xEF, 0xCD, 0xAB, 0x89}, {}, 500 * 1024 * 1024);
    addSig("Thumbcache DB", ".db", "Database", {0x56, 0x65, 0x72, 0x35, 0x46, 0x69, 0x6C}, {}, 50 * 1024 * 1024);
    // Archives / containers
    addSig("Apple DMG UDIF", ".dmg", "DiskImage", {0x78, 0x01, 0x73, 0x0D, 0x62, 0x70, 0x69, 0x73, 0x74}, {}, 50 * 1024 * 1024); // kolye block
    addSig("Sparse Image", ".sparseimage", "DiskImage", {0xE8, 0x5D, 0x9B, 0x53, 0x2D, 0x29, 0x2D, 0x21}, {}, 100 * 1024 * 1024);
    // Audio
    addSig("Opus Audio", ".opus", "Audio", {0x4F, 0x67, 0x67, 0x53}, {}, 50 * 1024 * 1024); // OGG container
    addSig("Musepack MPC", ".mpc", "Audio", {0x4D, 0x50, 0x2B, 0x05}, {}, 50 * 1024 * 1024);
    // Misc forensic artifacts
    addSig("Windows Prefetch", ".pf", "Misc", {0x4D, 0x41, 0x4D, 0x04}, {}, 1024 * 1024); // MAMx (Win10/11)
    addSig("LNK Shortcut", ".lnk", "Misc", {0x4C, 0x00, 0x00, 0x00, 0x01, 0x14, 0x02, 0x00}, {}, 1024 * 1024);
    addSig("EVTX Log", ".evtx", "Misc", {0x65, 0x6C, 0x69, 0x66}, {}, 100 * 1024 * 1024); // "elf" magic
}

} // namespace

bool CarvingEngine::loadSignatures(const std::string& jsonPath) {
    signatures.clear();
    loadEmbeddedSignatures(signatures);

    auto hexFn = [this](const std::string& hex) { return hexToBytes(hex); };
    if (!jsonPath.empty()) appendSignaturesFromJson(signatures, jsonPath, hexFn);
    appendSignaturesFromJson(signatures, "resources/signatures-extended.json", hexFn);
    appendSignaturesFromJson(signatures, "../resources/signatures-extended.json", hexFn);
    appendSignaturesFromJson(signatures, "../../resources/signatures-extended.json", hexFn);
    appendSignaturesFromJson(signatures, "signatures-extended.json", hexFn);

    for (size_t i = 0; i < signatures.size(); ++i) signatures[i].id = static_cast<int>(i);

    buildAhoCorasick();
    return !signatures.empty();
}

void CarvingEngine::buildAhoCorasick() {
    acNodes.clear();
    acNodes.emplace_back(); // root node at index 0
    maxPatternBytes_ = 64;

    // Add patterns to the trie
    for (const auto& sig : signatures) {
        maxPatternBytes_ = std::max(maxPatternBytes_, static_cast<uint32_t>(sig.header.size()));
        maxPatternBytes_ = std::max(maxPatternBytes_, static_cast<uint32_t>(sig.footer.size()));
        if (!sig.header.empty()) {
            int current = 0;
            for (uint8_t byte : sig.header) {
                if (acNodes[current].children.find(byte) == acNodes[current].children.end()) {
                    acNodes[current].children[byte] = (int)acNodes.size();
                    acNodes.emplace_back();
                }
                current = acNodes[current].children[byte];
            }
            acNodes[current].headerMatches.push_back(sig.id);
        }

        if (!sig.footer.empty()) {
            int current = 0;
            for (uint8_t byte : sig.footer) {
                if (acNodes[current].children.find(byte) == acNodes[current].children.end()) {
                    acNodes[current].children[byte] = (int)acNodes.size();
                    acNodes.emplace_back();
                }
                current = acNodes[current].children[byte];
            }
            acNodes[current].footerMatches.push_back(sig.id);
        }
    }

    // Build failure links using BFS
    std::queue<int> q;
    for (auto const& [byte, child] : acNodes[0].children) {
        acNodes[child].fail = 0;
        q.push(child);
    }

    while (!q.empty()) {
        int current = q.front();
        q.pop();

        for (auto const& [byte, child] : acNodes[current].children) {
            int failState = acNodes[current].fail;
            while (failState != 0 && acNodes[failState].children.find(byte) == acNodes[failState].children.end()) {
                failState = acNodes[failState].fail;
            }

            if (acNodes[failState].children.find(byte) != acNodes[failState].children.end()) {
                acNodes[child].fail = acNodes[failState].children[byte];
            } else {
                acNodes[child].fail = 0;
            }

            // Merge matches from the fail node
            acNodes[child].headerMatches.insert(
                acNodes[child].headerMatches.end(),
                acNodes[acNodes[child].fail].headerMatches.begin(),
                acNodes[acNodes[child].fail].headerMatches.end()
            );
            acNodes[child].footerMatches.insert(
                acNodes[child].footerMatches.end(),
                acNodes[acNodes[child].fail].footerMatches.begin(),
                acNodes[acNodes[child].fail].footerMatches.end()
            );

            q.push(child);
        }
    }
}

bool CarvingEngine::scan(DiskReader& reader, FileSystemParser::FileRecordCallback callback, std::atomic<bool>* isRunning) {
    if (!reader.isOpen() || signatures.empty() || acNodes.empty()) return false;
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    uint64_t maxSector = reader.getDiskSize() / sectorSize;
    return scanRange(reader, 0, maxSector, callback, isRunning);
}

bool CarvingEngine::scanRange(DiskReader& reader, uint64_t firstSector, uint64_t lastSector,
                              FileSystemParser::FileRecordCallback callback, std::atomic<bool>* isRunning) {
    if (!reader.isOpen() || signatures.empty() || acNodes.empty()) return false;
    if (lastSector <= firstSector) return false;

    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    const uint64_t rangeEndSector = std::min(lastSector, reader.getDiskSize() / sectorSize);
    const uint64_t totalSectors = rangeEndSector - firstSector;
    unsigned workers = carveWorkers_ > 0 ? carveWorkers_ : util::defaultCarveWorkers();
    const uint64_t minParallelSectors = 8192 * 4; // 16 MiB at 4K chunks
    if (workers <= 1 || totalSectors < minParallelSectors) {
        return scanRangeSingle(reader, firstSector, rangeEndSector, callback, isRunning, 0, 0, nullptr);
    }

    const uint64_t band = (totalSectors + workers - 1) / workers;
    const uint64_t overlapSectors =
        std::max<uint64_t>(1, (maxPatternBytes_ + sectorSize - 1) / sectorSize + 1);

    std::mutex callbackMu;
    std::atomic<int> sharedBgc{bgcBudget_};
    std::atomic<uint64_t> progressSector{firstSector};

    auto safeCallback = [&](const FileRecord& fr) {
        if (fr.id == -1) {
            uint64_t prev = progressSector.load(std::memory_order_relaxed);
            while (fr.startSector > prev &&
                   !progressSector.compare_exchange_weak(prev, fr.startSector, std::memory_order_relaxed)) {}
            std::lock_guard<std::mutex> lock(callbackMu);
            FileRecord tick;
            tick.id = -1;
            tick.startSector = progressSector.load(std::memory_order_relaxed);
            callback(tick);
            return;
        }
        std::lock_guard<std::mutex> lock(callbackMu);
        callback(fr);
    };

    util::parallelFor(workers, [&](unsigned w, unsigned n) {
        const uint64_t coreStart = firstSector + w * band;
        const uint64_t coreEnd = std::min(rangeEndSector, coreStart + band);
        if (coreStart >= coreEnd) return;

        uint64_t scanStart = coreStart;
        if (w > 0 && coreStart > overlapSectors) scanStart = coreStart - overlapSectors;
        uint64_t scanEnd = coreEnd;
        if (w + 1 < n && coreEnd + overlapSectors < rangeEndSector) {
            scanEnd = coreEnd + overlapSectors;
        }

        scanRangeSingle(reader, scanStart, scanEnd, safeCallback, isRunning,
                        coreStart, coreEnd, &sharedBgc);
    });

    FileRecord done;
    done.id = -1;
    done.startSector = rangeEndSector;
    callback(done);
    return true;
}

bool CarvingEngine::scanRangeSingle(DiskReader& reader, uint64_t firstSector, uint64_t lastSector,
                                    FileSystemParser::FileRecordCallback callback,
                                    std::atomic<bool>* isRunning,
                                    uint64_t emitFirstSector, uint64_t emitLastSector,
                                    std::atomic<int>* bgcBudget) {
    if (!reader.isOpen() || signatures.empty() || acNodes.empty()) return false;
    if (lastSector <= firstSector) return false;

    uint64_t diskSize = reader.getDiskSize();
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;

    auto emit = [&](const FileRecord& fr) {
        if (emitLastSector > emitFirstSector && fr.id >= 0) {
            if (fr.startSector < emitFirstSector || fr.startSector >= emitLastSector) return;
        }
        callback(fr);
    };
    
    const uint32_t chunkSectors = 8192; // 4MB chunks
    const uint32_t chunkSize = chunkSectors * sectorSize;
    auto poolBufA = MemoryPool::getInstance().acquireBuffer(chunkSize);
    auto* currentBuf = poolBufA.get();
    
    uint64_t rangeEndSector = std::min(lastSector, diskSize / sectorSize);
    int foundCount = 0;
    
    int currentState = 0;
    std::vector<ActiveCarve> activeCarves;
    
    for (uint64_t sector = firstSector; sector < rangeEndSector; sector += chunkSectors) {
        if (isRunning && !(*isRunning)) break;
        
        auto res = reader.readSectors(sector * sectorSize, chunkSize, currentBuf->data());
        if (!res.success) continue;
        
        uint64_t baseOffset = sector * sectorSize;

        for (uint32_t i = 0; i < res.bytesRead; ++i) {
            uint8_t byte = currentBuf->data()[i];
            
            while (currentState != 0 && acNodes[currentState].children.find(byte) == acNodes[currentState].children.end()) {
                currentState = acNodes[currentState].fail;
            }
            if (acNodes[currentState].children.find(byte) != acNodes[currentState].children.end()) {
                currentState = acNodes[currentState].children[byte];
            } else {
                currentState = 0;
            }
            
            uint64_t currentAbsoluteOffset = baseOffset + i;

            // Handle Header Matches
            for (int sigId : acNodes[currentState].headerMatches) {
                const auto& sig = signatures[sigId];

                // Avoid duplicates for the same signature that overlap
                bool alreadyActive = false;
                for (const auto& ac : activeCarves) {
                    if (ac.sigId == sigId && currentAbsoluteOffset - ac.startOffset < 4096) {
                        alreadyActive = true;
                        break;
                    }
                }
                // CA-006: signatures that share a magic (leftover collisions)
                // must not open a SECOND carve at the same bytes — one
                // candidate per start offset, whatever the family.
                if (!alreadyActive) {
                    uint64_t thisStart = currentAbsoluteOffset - sig.header.size() + 1;
                    for (const auto& ac : activeCarves) {
                        if (thisStart >= ac.startOffset && thisStart - ac.startOffset < 512) {
                            alreadyActive = true;
                            break;
                        }
                    }
                }

                if (!alreadyActive) {
                    ActiveCarve ac;
                    ac.sigId = sigId;
                    ac.startOffset = currentAbsoluteOffset - sig.header.size() + 1;
                    ac.startSector = ac.startOffset / sectorSize;
                    ac.endOffsetLimit = ac.startOffset + sig.maxSize;
                    ac.filename = "carved_" + std::to_string(foundCount++) + "_" + std::to_string(ac.startSector) + sig.extension;
                    activeCarves.push_back(ac);
                }
            }
            
            // Handle Footer Matches
            for (int sigId : acNodes[currentState].footerMatches) {
                const auto& sig = signatures[sigId];
                
                // Find matching active carve
                auto it = activeCarves.begin();
                while (it != activeCarves.end()) {
                    if (it->sigId == sigId) {
                        uint64_t fileEndOffset = currentAbsoluteOffset + 1; // End of footer
                        if (fileEndOffset <= it->endOffsetLimit) {
                            uint64_t actualSize = fileEndOffset - it->startOffset;
                            std::string ext = sig.extension.empty() ? "" : sig.extension.substr(1);

                            // Fast Object Validation: read the carved span back
                            // from disk and run the structural validator for this
                            // type. A high score confirms the header/footer match
                            // was a real file; a low score means the magic bytes
                            // coincidentally appeared in unrelated data and the
                            // candidate should be down-ranked. Capped at 1 MiB so
                            // very large carves do not stall the scan.
                            int confidence = 95; // header+footer baseline
                            // CA-004 fix: when the structural validator returns a
                            // partial score (fragmented or damaged candidate), run
                            // Bifragmented Gap Carving over the carved span. If
                            // removing one contiguous gap makes the object
                            // validate, report the two fragments as data runs so
                            // recovery stitches them correctly.
                            // CA-001: sector-step BGC; per-scan attempt budget.
                            // Gap size is the caller max (span/4 below), no 64 KiB clamp.
                            bool bgcRescued = false;
                            BgcResult bgc{};
                            if ((bgcBudget ? bgcBudget->load() : bgcBudget_) > 0 &&
                                actualSize > 4096 && actualSize <= (16u << 20)) {
                                uint32_t alignedSize = ((static_cast<uint32_t>(actualSize) + sectorSize - 1) / sectorSize) * sectorSize;
                                std::vector<uint8_t> alignedBuf(alignedSize);
                                auto rres = reader.readSectors(it->startOffset, alignedSize, alignedBuf.data());
                                if (rres.success && rres.bytesRead >= actualSize) {
                                    confidence = dispatchValidator(ext, alignedBuf.data(), static_cast<size_t>(actualSize));
                                    if (confidence >= 40 && confidence < 85) {
                                        if (bgcBudget) bgcBudget->fetch_sub(1);
                                        else --bgcBudget_;
                                        bgc = bifragmentedGapCarve(
                                            alignedBuf.data(), static_cast<size_t>(actualSize),
                                            0, static_cast<size_t>(actualSize),
                                            static_cast<size_t>(actualSize) / 4,
                                            [&ext](const uint8_t* d, size_t n) {
                                                return dispatchValidator(ext, d, n);
                                            },
                                            sectorSize);
                                        if (!bgc.found) {
                                            bgc = triFragmentedGapCarve(
                                                alignedBuf.data(), static_cast<size_t>(actualSize),
                                                0, static_cast<size_t>(actualSize),
                                                static_cast<size_t>(actualSize) / 4,
                                                [&ext](const uint8_t* d, size_t n) {
                                                    return dispatchValidator(ext, d, n);
                                                },
                                                sectorSize);
                                        }
                                        bgcRescued = bgc.found;
                                    }
                                }
                            } else if (actualSize > 0) {
                                // Above the BGC cap: keep the header/footer
                                // baseline; nothing else to check cheaply.
                                confidence = 70;
                            }

                            // CA-006: resolve the RIFF subtype into the real
                            // extension so the record is named what it is.
                            // Independent 512-byte header read — works for
                            // candidates of every size, not just the BGC window.
                            std::string effExt = ext;
                            std::string effName = it->filename;
                            if (isZipFamilyExt(effExt) || effExt == "sqlite" || effExt == "db" ||
                                isMp4FamilyExt(effExt)) {
                                uint32_t probe = static_cast<uint32_t>(std::min<uint64_t>(actualSize, 1u << 20));
                                probe = ((probe + sectorSize - 1) / sectorSize) * sectorSize;
                                if (probe > 0) {
                                    std::vector<uint8_t> probeBuf(probe);
                                    if (reader.readSectors(it->startOffset, probe, probeBuf.data()).success) {
                                        applyStructuralRefinement(effExt, probeBuf.data(),
                                                                  static_cast<size_t>(actualSize),
                                                                  actualSize, effExt, confidence);
                                        auto dot = effName.find_last_of('.');
                                        if (dot != std::string::npos) effName = effName.substr(0, dot);
                                        effName += std::string(".") + effExt;
                                    }
                                }
                            }
                            if (ext == "riff") {
                                uint8_t hdr[512];
                                uint32_t hdrAligned = ((512u + sectorSize - 1) / sectorSize) * sectorSize;
                                std::vector<uint8_t> hdrBuf(hdrAligned);
                                if (reader.readSectors(it->startOffset, hdrAligned, hdrBuf.data()).success) {
                                    std::memcpy(hdr, hdrBuf.data(), 512);
                                    if (const char* sub = byteback::carver::detectRiffSubtype(hdr, 512)) {
                                        effExt = sub;
                                        auto dot = effName.find_last_of('.');
                                        if (dot != std::string::npos) effName = effName.substr(0, dot);
                                        effName += std::string(".") + sub;
                                    }
                                }
                            }
                            if (bgcRescued) {
                                std::string effExtBgc = effExt;
                                std::string effNameBgc = effName; // already subtype-resolved above
                                FileRecord fr;
                                fr.id = 0;
                                fr.parentId = 0;
                                fr.name = effNameBgc;
                                fr.extension = effExtBgc;
                                fr.path = "/recovered_raw/" + fr.name;
                                fr.sizeBytes = actualSize - bgc.gapLen - bgc.gap2Len;
                                fr.startSector = it->startSector;
                                fr.endSector = (fileEndOffset + sectorSize - 1) / sectorSize;
                                fr.runs.push_back({it->startOffset / sectorSize,
                                                   (bgc.frag1Len + sectorSize - 1) / sectorSize});
                                uint64_t frag2Start = it->startOffset + bgc.frag1Len + bgc.gapLen;
                                if (bgc.frag2Len > 0 || bgc.gap2Len > 0) {
                                    fr.runs.push_back({frag2Start / sectorSize,
                                                       (bgc.frag2Len + sectorSize - 1) / sectorSize});
                                    uint64_t frag3Start = frag2Start + bgc.frag2Len + bgc.gap2Len;
                                    fr.runs.push_back({frag3Start / sectorSize,
                                                       (actualSize - bgc.frag1Len - bgc.gapLen -
                                                        bgc.frag2Len - bgc.gap2Len + sectorSize - 1) /
                                                           sectorSize});
                                } else {
                                    fr.runs.push_back({frag2Start / sectorSize,
                                                       (actualSize - bgc.frag1Len - bgc.gapLen + sectorSize - 1) /
                                                           sectorSize});
                                }
                                fr.status = 0;
                                fr.confidence = 88; // BGC-validated reassembly
                                fr.category = sig.category;
                                fr.source = "carver_bgc";
                                fr.createdAt = 0;
                                fr.modifiedAt = 0;

                                emit(fr);

                                it = activeCarves.erase(it);
                                continue;
                            }

                            FileRecord fr;
                            fr.id = 0;
                            fr.parentId = 0;
                            fr.name = effName;
                            fr.extension = effExt;
                            fr.path = "/recovered_raw/" + fr.name;
                            fr.sizeBytes = actualSize;
                            fr.startSector = it->startSector;
                            fr.endSector = (fileEndOffset + sectorSize - 1) / sectorSize;
                            fr.status = 0;
                            fr.confidence = confidence;
                            fr.category = sig.category;
                            fr.source = "carver";
                            fr.createdAt = 0;
                            fr.modifiedAt = 0;
                            
                            emit(fr);
                            
                            // Remove from active carves
                            it = activeCarves.erase(it);
                            continue;
                        }
                    }
                    ++it;
                }
            }
        }
        
        // Prune expired active carves
        uint64_t currentOffsetEndOfChunk = baseOffset + res.bytesRead;
        auto it = activeCarves.begin();
        while (it != activeCarves.end()) {
            if (currentOffsetEndOfChunk > it->endOffsetLimit) {
                const auto& sig = signatures[it->sigId];
                std::string effExt = sig.extension.empty() ? "" : sig.extension.substr(1);
                uint64_t actualSize = sig.maxSize;
                int confidence = 70;
                uint32_t probe = static_cast<uint32_t>(std::min<uint64_t>(sig.maxSize, 1u << 20));
                probe = ((probe + sectorSize - 1) / sectorSize) * sectorSize;
                std::vector<uint8_t> probeBuf;
                if (probe > 0) {
                    probeBuf.resize(probe);
                    if (reader.readSectors(it->startOffset, probe, probeBuf.data()).success) {
                        applyStructuralRefinement(effExt, probeBuf.data(), probeBuf.size(),
                                                  actualSize, effExt, confidence);
                    }
                }

                FileRecord fr;
                fr.id = 0;
                fr.parentId = 0;
                fr.name = it->filename;
                fr.extension = effExt;
                fr.path = "/recovered_raw/" + fr.name;
                fr.sizeBytes = actualSize;
                fr.startSector = it->startSector;
                fr.endSector = it->startSector + (actualSize + sectorSize - 1) / sectorSize;
                fr.status = 0;
                fr.confidence = confidence;
                fr.category = refineCarveCategory(probeBuf.empty() ? nullptr : probeBuf.data(),
                                                  probeBuf.size(), effExt, sig.category);
                fr.source = "carver";
                fr.createdAt = 0;
                fr.modifiedAt = 0;
                
                emit(fr);
                
                it = activeCarves.erase(it);
            } else {
                ++it;
            }
        }
        
        FileRecord progressTick;
        progressTick.id = -1;
        progressTick.startSector = sector + chunkSectors;
        emit(progressTick);
    }

    // Process remaining active carves when disk ends
    uint64_t endOfDiskOffset = std::min(diskSize, rangeEndSector * sectorSize);
    for (const auto& ac : activeCarves) {
        const auto& sig = signatures[ac.sigId];
        uint64_t actualSize = std::min(sig.maxSize, endOfDiskOffset > ac.startOffset ? endOfDiskOffset - ac.startOffset : 0);
        std::string effExt = sig.extension.empty() ? "" : sig.extension.substr(1);
        int confidence = 70;
        uint32_t probe = static_cast<uint32_t>(std::min<uint64_t>(actualSize, 1u << 20));
        probe = ((probe + sectorSize - 1) / sectorSize) * sectorSize;
        std::vector<uint8_t> probeBuf;
        if (probe > 0) {
            probeBuf.resize(probe);
            if (reader.readSectors(ac.startOffset, probe, probeBuf.data()).success) {
                applyStructuralRefinement(effExt, probeBuf.data(), probeBuf.size(),
                                          actualSize, effExt, confidence);
            }
        }

        FileRecord fr;
        fr.id = 0;
        fr.parentId = 0;
        fr.name = ac.filename;
        fr.extension = effExt;
        fr.path = "/recovered_raw/" + fr.name;
        fr.sizeBytes = actualSize;
        fr.startSector = ac.startSector;
        fr.endSector = ac.startSector + (actualSize + sectorSize - 1) / sectorSize;
        fr.status = 0;
        fr.confidence = confidence;
        fr.category = refineCarveCategory(probeBuf.empty() ? nullptr : probeBuf.data(), probeBuf.size(),
                                        effExt, sig.category);
        fr.source = "carver";
        fr.createdAt = 0;
        fr.modifiedAt = 0;
        
        emit(fr);
    }

    return true;
}

} // namespace byteback
