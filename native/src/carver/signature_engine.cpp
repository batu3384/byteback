#include "byteback_carver.h"
#include "byteback_memory.h"
#include "carver/file_validators.h"
#include "carver/structural_parsers.h"
#include "carver/content_classifier.h"
#include "carver/embedded_metadata.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <regex>
#include <functional>
#include <mutex>

namespace byteback {

namespace {
// Dispatch a carved buffer to the right Fast Object Validator based on the
// signature's extension. Returns the validator's [0,100] confidence, or 55
// when no structural validator exists (header/footer alone is not certainty).
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
    if (ext == "bmp")                   return validateBmp(data, size);
    if (ext == "ts")                    return validateMpegTs(data, size);
    if (ext == "sqlite" || ext == "db") return carver::validateSqlite(data, size);
    if (ext == "mp4" || ext == "mov" || ext == "m4v" || ext == "m4a" ||
        ext == "qt" || ext == "3gp" || ext == "heic" || ext == "heif" ||
        ext == "avif" || ext == "cr3")   return carver::validateMp4(data, size);
    return 55; // no structural validator — header/footer only, not "almost certain"
}

bool isZipFamilyExt(const std::string& ext) {
    return ext == "zip" || ext == "docx" || ext == "xlsx" || ext == "pptx" ||
           ext == "odt" || ext == "ods" || ext == "odp" || ext == "epub" || ext == "jar";
}

bool isMp4FamilyExt(const std::string& ext) {
    return ext == "mp4" || ext == "mov" || ext == "m4v" || ext == "m4a" ||
           ext == "qt" || ext == "3gp" || ext == "heic" || ext == "heif" ||
           ext == "avif" || ext == "cr3";
}

void applyStructuralRefinement(const std::string& ext, const uint8_t* data, size_t size,
                               uint64_t& actualSize, std::string& effExt, int& confidence) {
    carver::StructuralParseResult pr;
    if (isZipFamilyExt(ext)) pr = carver::parseZipFamily(data, size);
    else if (ext == "sqlite" || ext == "db") pr = carver::parseSqliteDb(data, size);
    else if (isMp4FamilyExt(ext)) pr = carver::parseMp4Mov(data, size);
    else if (ext == "tiff" || ext == "cr2") pr = carver::parseTiff(data, size);
    else if (ext == "riff") pr = carver::parseRiff(data, size);
    else if (ext == "ts") pr = carver::parseMpegTs(data, size);
    else if (ext == "7z") pr = carver::parseSevenZip(data, size);
    else if (ext == "cab") pr = carver::parseCab(data, size);
    else return;

    if (!pr.valid) return;
    if (pr.size > 0 && pr.size <= actualSize) actualSize = pr.size;
    if (!pr.extension.empty()) effExt = pr.extension;
    if (pr.confidence > confidence) confidence = pr.confidence;
}

// Weak BM magic: drop clear FPs; demote shaky hits to .bin.
bool refineBmpCarve(const uint8_t* data, size_t size, std::string& filename,
                    std::string& effExt, uint64_t& actualSize, int& confidence) {
    if (effExt != "bmp") return true;
    const int score = (data && size) ? carver::validateBmp(data, size) : 0;
    confidence = score;
    if (score < 50) return false;
    if (data && size >= 6) {
        const uint32_t bfSize = static_cast<uint32_t>(data[2]) | (static_cast<uint32_t>(data[3]) << 8) |
                                (static_cast<uint32_t>(data[4]) << 16) | (static_cast<uint32_t>(data[5]) << 24);
        if (bfSize >= 14 && bfSize <= actualSize) actualSize = bfSize;
    }
    if (score < 70) {
        const auto dot = filename.find_last_of('.');
        if (dot != std::string::npos) filename = filename.substr(0, dot) + ".bin";
        else filename += ".bin";
        effExt = "bin";
    }
    return true;
}

// Carve records store EXIF in modifiedAt; UI labels as EXIF not FS date.
void stampCarveExif(FileRecord& fr, const std::string& effExt, DiskReader& reader,
                    uint64_t startOff, uint64_t fileSize, const uint8_t* cached, size_t cachedLen) {
    if (effExt != "jpg" && effExt != "jpeg") return;
    int64_t t = 0;
    if (cached && cachedLen > 0) {
        t = carver::extractJpegExifUnix(cached, std::min(cachedLen, size_t{65536}));
    }
    if (t <= 0 && fileSize > 0) {
        uint32_t ss = reader.getSectorSize();
        if (ss == 0) ss = 512;
        uint32_t want = static_cast<uint32_t>(std::min<uint64_t>(fileSize, 65536));
        want = ((want + ss - 1) / ss) * ss;
        if (want == 0) want = 512;
        std::vector<uint8_t> head(want);
        // CA-004: startOff is byte-exact and may sit mid-sector; readBytes
        // handles the alignment instead of rejecting the read.
        if (reader.readBytes(startOff, want, head.data()).success) {
            t = carver::extractJpegExifUnix(head.data(), std::min(static_cast<size_t>(want), static_cast<size_t>(fileSize)));
        }
    }
    if (t > 0) fr.modifiedAt = t;
}

// Expire / disk-end carves without a footer match: validate before emit.
// CA-001 policy: the size must be bounded by a structural parser (zip EOCD,
// mp4 atoms, TIFF strips, RIFF chunk size, ...). Candidates still sitting at
// the signature's maxSize are phantom records — the garbage generator — and
// are dropped regardless of validator score.
bool refineExpiredCarve(const FileSignature& sig, DiskReader* reader, uint64_t carveStartOffset,
                        const uint8_t* data, size_t probeSize,
                        std::string& filename, std::string& effExt, uint64_t& actualSize,
                        int& confidence) {
    const uint64_t unboundedSize = actualSize;
    if (!refineBmpCarve(data, probeSize, filename, effExt, actualSize, confidence)) return false;

    const int vScore = (data && probeSize) ? dispatchValidator(effExt, data, probeSize) : 0;
    if (vScore > 0) confidence = vScore;
    else if (sig.footer.empty()) confidence = 55; // ponytail: header-only ceiling, not 70

    if (reader && isMp4FamilyExt(effExt)) {
        // CA-018: box walk with targeted reads — a 2GB movie is no longer
        // clamped to the 1MB probe.
        auto readAt = [reader](uint64_t off, uint32_t len, uint8_t* out) {
            auto res = reader->readBytes(off, len, out);
            return res.success && res.bytesRead >= len;
        };
        auto pr = carver::parseIsobmffBounded(data, probeSize, carveStartOffset,
                                              unboundedSize, readAt);
        if (pr.valid) {
            if (pr.size > 0 && pr.size <= actualSize) actualSize = pr.size;
            if (!pr.extension.empty()) effExt = pr.extension;
            if (pr.confidence > confidence) confidence = pr.confidence;
        }
    } else {
        applyStructuralRefinement(effExt, data, probeSize, actualSize, effExt, confidence);
    }

    if (effExt == "riff" && data && probeSize >= 12) {
        if (const char* sub = carver::detectRiffSubtype(data, probeSize)) {
            effExt = sub;
            const int rs = dispatchValidator(effExt, data, probeSize);
            if (rs > confidence) confidence = rs;
        }
    }

    if (!data || probeSize == 0) return false;
    if (actualSize >= unboundedSize) return false; // size never bounded -> phantom

    if (sig.footer.empty()) {
        if (vScore > 0 && vScore < 50) return false;
    } else if (vScore > 0 && vScore < 45) {
        return false;
    }
    return true;
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
    // Images
    // ============================================================
    addSig("JPEG Image", ".jpg", "Image", {0xFF, 0xD8, 0xFF}, {0xFF, 0xD9}, 256 * 1024 * 1024);
    addSig("PNG Image", ".png", "Image", {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}, {0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82}, 256 * 1024 * 1024);
    addSig("GIF Image", ".gif", "Image", {0x47, 0x49, 0x46, 0x38}, {0x00, 0x3B}, 64 * 1024 * 1024);
    // BM alone is too weak; expire-path validateBmp rejects / demotes FPs.
    addSig("BMP Image", ".bmp", "Image", {0x42, 0x4D}, {}, 50 * 1024 * 1024);
    // CA-006: specific RAW before generic TIFF — same-offset dedup keeps the
    // first match, so the 10-byte CR2 magic must precede the 4-byte TIFF one.
    addSig("Canon CR2 RAW", ".cr2", "Image", {0x49, 0x49, 0x2A, 0x00, 0x10, 0x00, 0x00, 0x00, 0x43, 0x52}, {}, 50 * 1024 * 1024);
    addSig("TIFF Image (LE)", ".tiff", "Image", {0x49, 0x49, 0x2A, 0x00}, {}, 100 * 1024 * 1024);
    addSig("TIFF Image (BE)", ".tiff", "Image", {0x4D, 0x4D, 0x00, 0x2A}, {}, 100 * 1024 * 1024);
    // CA-006: single RIFF-container signature. The subtype (WEBP/AVI/WAVE)
    // lives at bytes 8..11 and is resolved by validateRiff at emit time —
    // the old trio of same-magic signatures triple-reported every RIFF file.
    addSig("RIFF Container", ".riff", "Container", {0x52, 0x49, 0x46, 0x46}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("HEIC Image", ".heic", "Image", {0x00, 0x00, 0x00, 0x18, 0x66, 0x74, 0x79, 0x70, 0x68, 0x65, 0x69, 0x63}, {}, 256 * 1024 * 1024);

    // ============================================================
    // Documents
    // ============================================================
    addSig("PDF Document", ".pdf", "Document", {0x25, 0x50, 0x44, 0x46, 0x2D}, {0x25, 0x25, 0x45, 0x4F, 0x46}, 100 * 1024 * 1024);
    addSig("RTF Document", ".rtf", "Document", {0x7B, 0x5C, 0x72, 0x74, 0x66, 0x31}, {0x7D}, 20 * 1024 * 1024);
    addSig("MS Office (OLE2)", ".doc", "Document", {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1}, {}, 100 * 1024 * 1024);
    // CA-002: doc/xls/ppt/msi/msg all share the OLE2 magic above; JSON entries
    // per product were removed (same-offset dedup kept whichever loaded first,
    // mislabeling the rest). Text-magic "signatures" (Markdown/XML/HTML/LaTeX/
    // SVG/MBOX/EXE MZ/...) are gone entirely — they matched ordinary text and
    // opened maxSize-sized phantom carves, which is what flooded results.

    // ============================================================
    // Video
    // ============================================================
    // Generic ftyp catch for brands not listed in appendFtypMediaSignatures
    // (which run first and win the same-offset dedup).
    addSig("MP4 Video", ".mp4", "Video", {0x00, 0x00, 0x00, 0x18, 0x66, 0x74, 0x79, 0x70}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("MKV/WebM Video", ".mkv", "Video", {0x1A, 0x45, 0xDF, 0xA3}, {}, 4ULL * 1024 * 1024 * 1024);
    addSig("MPEG Video", ".mpg", "Video", {0x00, 0x00, 0x01, 0xBA}, {0x00, 0x00, 0x01, 0xB9}, 2ULL * 1024 * 1024 * 1024);
    addSig("FLV Video", ".flv", "Video", {0x46, 0x4C, 0x56, 0x01}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("ASF Video (WMV)", ".wmv", "Video", {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("3GP Video", ".3gp", "Video", {0x00, 0x00, 0x00, 0x14, 0x66, 0x74, 0x79, 0x70, 0x33, 0x67, 0x70}, {}, 1ULL * 1024 * 1024 * 1024);
    addSig("MPEG-TS", ".ts", "Video", {0x47, 0x40, 0x00}, {}, 4ULL * 1024 * 1024 * 1024);
    addSig("SWF Flash", ".swf", "Video", {0x46, 0x57, 0x53}, {}, 100 * 1024 * 1024);

    // ============================================================
    // Audio
    // ============================================================
    addSig("MP3 Audio", ".mp3", "Audio", {0x49, 0x44, 0x33}, {}, 128 * 1024 * 1024);
    addSig("FLAC Audio", ".flac", "Audio", {0x66, 0x4C, 0x61, 0x43}, {}, 100 * 1024 * 1024);
    addSig("OGG Audio", ".ogg", "Audio", {0x4F, 0x67, 0x67, 0x53}, {}, 100 * 1024 * 1024);
    addSig("MIDI Audio", ".mid", "Audio", {0x4D, 0x54, 0x68, 0x64}, {}, 5 * 1024 * 1024);
    addSig("AIFF Audio", ".aiff", "Audio", {0x46, 0x4F, 0x52, 0x4D}, {}, 100 * 1024 * 1024);
    addSig("APE Audio", ".ape", "Audio", {0x4D, 0x41, 0x43, 0x20}, {}, 100 * 1024 * 1024);
    addSig("WavPack Audio", ".wv", "Audio", {0x77, 0x76, 0x70, 0x6B}, {}, 100 * 1024 * 1024);
    addSig("Musepack MPC", ".mpc", "Audio", {0x4D, 0x50, 0x2B, 0x05}, {}, 50 * 1024 * 1024);

    // ============================================================
    // Archives
    // ============================================================
    addSig("ZIP/DOCX/XLSX", ".zip", "Archive", {0x50, 0x4B, 0x03, 0x04}, {0x50, 0x4B, 0x05, 0x06}, 500 * 1024 * 1024);
    addSig("RAR Archive", ".rar", "Archive", {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00}, {}, 500 * 1024 * 1024);
    addSig("RAR Archive v5", ".rar", "Archive", {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00}, {}, 500 * 1024 * 1024);
    addSig("7-Zip Archive", ".7z", "Archive", {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C}, {}, 500 * 1024 * 1024);
    addSig("GZIP Archive", ".gz", "Archive", {0x1F, 0x8B, 0x08}, {}, 100 * 1024 * 1024);
    addSig("BZIP2 Archive", ".bz2", "Archive", {0x42, 0x5A, 0x68}, {}, 100 * 1024 * 1024);
    addSig("XZ Archive", ".xz", "Archive", {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00}, {}, 500 * 1024 * 1024);
    addSig("ZSTD Archive", ".zst", "Archive", {0x28, 0xB5, 0x2F, 0xFD}, {}, 500 * 1024 * 1024);
    addSig("LZ4 Archive", ".lz4", "Archive", {0x04, 0x22, 0x4D, 0x18}, {}, 500 * 1024 * 1024);
    addSig("CAB Archive", ".cab", "Archive", {0x4D, 0x53, 0x43, 0x46}, {}, 500 * 1024 * 1024);

    // ============================================================
    // Databases & Email
    // ============================================================
    addSig("SQLite Database", ".sqlite", "Database", {0x53, 0x51, 0x4C, 0x69, 0x74, 0x65, 0x20, 0x66, 0x6F, 0x72, 0x6D, 0x61, 0x74, 0x20, 0x33, 0x00}, {}, 1ULL * 1024 * 1024 * 1024);
    addSig("PST/OST Email Data", ".pst", "Email", {0x21, 0x42, 0x44, 0x4E}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("MS Access DB", ".mdb", "Database", {0x00, 0x01, 0x00, 0x00, 0x53, 0x74, 0x61, 0x6E, 0x64, 0x61, 0x72, 0x64, 0x20, 0x4A, 0x65, 0x74}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("MS Access 2007+", ".accdb", "Database", {0x00, 0x01, 0x00, 0x00, 0x53, 0x74, 0x61, 0x6E, 0x64, 0x61, 0x72, 0x64, 0x20, 0x41, 0x43, 0x45}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("Windows Registry", ".reg", "System", {0x72, 0x65, 0x67, 0x66}, {}, 100 * 1024 * 1024);
    addSig("Windows Shortcut", ".lnk", "System", {0x4C, 0x00, 0x00, 0x00, 0x01, 0x14, 0x02, 0x00}, {}, 1 * 1024 * 1024);

    // ============================================================
    // System & Forensics
    // ============================================================
    addSig("NTFS MFT Record", ".mft", "System", {0x46, 0x49, 0x4C, 0x45, 0x30}, {}, 1024); // MFT FILE0 record is typically 1024 bytes
    addSig("Windows EDB", ".edb", "Database", {0xEF, 0xCD, 0xAB, 0x89}, {}, 500 * 1024 * 1024);
    addSig("Thumbcache DB", ".db", "Database", {0x56, 0x65, 0x72, 0x35, 0x46, 0x69, 0x6C}, {}, 512 * 1024 * 1024);
    addSig("Windows Prefetch", ".pf", "Misc", {0x4D, 0x41, 0x4D, 0x04}, {}, 1024 * 1024); // MAMx (Win10/11)
    // Real EVTX magic is "ElfFile\0" (the old "elif" matched every Python file).
    addSig("EVTX Log", ".evtx", "Misc", {0x45, 0x6C, 0x66, 0x46, 0x69, 0x6C, 0x65, 0x00}, {}, 100 * 1024 * 1024);

    // ============================================================
    // Disk Images & Virtualization
    // ============================================================
    addSig("VHD Disk Image", ".vhd", "DiskImage", {0x63, 0x6F, 0x6E, 0x65, 0x63, 0x74, 0x69, 0x78}, {}, 100ULL * 1024 * 1024 * 1024);
    addSig("VMDK Disk Image", ".vmdk", "DiskImage", {0x4B, 0x44, 0x4D, 0x56}, {}, 100ULL * 1024 * 1024 * 1024);
    addSig("QCOW2 Disk Image", ".qcow2", "DiskImage", {0x51, 0x46, 0x49, 0xFB}, {}, 100ULL * 1024 * 1024 * 1024);
    addSig("VDI VirtualBox", ".vdi", "DiskImage", {0x3C, 0x3C, 0x3C, 0x20}, {}, 100ULL * 1024 * 1024 * 1024);

    // ============================================================
    // Fonts
    // ============================================================
    addSig("TrueType Font", ".ttf", "Font", {0x00, 0x01, 0x00, 0x00, 0x00}, {}, 10 * 1024 * 1024);
    addSig("OpenType Font", ".otf", "Font", {0x4F, 0x54, 0x54, 0x4F}, {}, 10 * 1024 * 1024);
    addSig("WOFF Font", ".woff", "Font", {0x77, 0x4F, 0x46, 0x46}, {}, 10 * 1024 * 1024);
    addSig("WOFF2 Font", ".woff2", "Font", {0x77, 0x4F, 0x46, 0x32}, {}, 10 * 1024 * 1024);

    // ============================================================
    // Misc & forensic artifacts
    // ============================================================
    addSig("PCap Network", ".pcap", "Network", {0xD4, 0xC3, 0xB2, 0xA1}, {}, 1ULL * 1024 * 1024 * 1024);
    addSig("PCap-ng Network", ".pcapng", "Network", {0x0A, 0x0D, 0x0D, 0x0A}, {}, 1ULL * 1024 * 1024 * 1024);
    addSig("Fuji RAF RAW", ".raf", "Image", {0x46, 0x55, 0x4A, 0x49, 0x46, 0x49, 0x4C, 0x4D, 0x43, 0x43, 0x44, 0x44, 0x2D, 0x52, 0x41, 0x57}, {}, 100 * 1024 * 1024);
    addSig("JPEG2000 JP2", ".jp2", "Image", {0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A}, {}, 50 * 1024 * 1024);
    addSig("Apple Sparse Image", ".sparseimage", "DiskImage", {0xE8, 0x5D, 0x9B, 0x53, 0x2D, 0x29, 0x2D, 0x21}, {}, 100 * 1024 * 1024);
    addSig("KeePass KDBX", ".kdbx", "Document", {0x03, 0x4B, 0x44, 0x42, 0x58}, {}, 50 * 1024 * 1024);
    addSig("OneNote Package", ".one", "Document", {0xE4, 0x52, 0x5C, 0x7B, 0x8C, 0xD9, 0xA7, 0x4D}, {}, 500 * 1024 * 1024);
    addSig("Adobe InDesign", ".indd", "Document", {0x06, 0x06, 0xED, 0xFD, 0xD5, 0x06, 0xE2, 0x17}, {}, 500 * 1024 * 1024);
    addSig("Blender Blend", ".blend", "Document", {0x42, 0x4C, 0x45, 0x4E, 0x44, 0x45, 0x52}, {}, 500 * 1024 * 1024);
}

void appendFtypMediaSignatures(std::vector<FileSignature>& signatures) {
    struct BrandRow {
        const char* fmt;
        const char* ext;
        const char* cat;
        const char* brand; // 4-char major brand
        uint32_t boxSize;
        uint64_t maxS;
    };
    static const BrandRow rows[] = {
        {"HEIF heif", ".heif", "Image", "heif", 0x18, 256u << 20},
        {"HEIF mif1", ".heif", "Image", "mif1", 0x18, 256u << 20},
        {"HEIC hevc", ".heic", "Image", "hevc", 0x18, 256u << 20},
        {"HEIC heix", ".heic", "Image", "heix", 0x18, 256u << 20},
        {"HEIC hev1", ".heic", "Image", "hev1", 0x18, 256u << 20},
        {"HEIC msf1", ".heic", "Image", "msf1", 0x18, 256u << 20},
        {"AVIF avif", ".avif", "Image", "avif", 0x20, 256u << 20},
        {"AVIF avis", ".avif", "Image", "avis", 0x20, 256u << 20},
        {"MP4 isom", ".mp4", "Video", "isom", 0x18, 4ULL << 30},
        {"MP4 iso2", ".mp4", "Video", "iso2", 0x18, 4ULL << 30},
        {"MP4 mp41", ".mp4", "Video", "mp41", 0x18, 4ULL << 30},
        {"MP4 mp42", ".mp4", "Video", "mp42", 0x18, 4ULL << 30},
        {"MP4 avc1", ".mp4", "Video", "avc1", 0x18, 4ULL << 30},
        {"MP4 ndas", ".mp4", "Video", "ndas", 0x18, 4ULL << 30},
        {"MP4 dash", ".mp4", "Video", "dash", 0x18, 4ULL << 30},
        {"M4V video", ".m4v", "Video", "M4V ", 0x1C, 4ULL << 30},
        {"M4A audio", ".m4a", "Audio", "M4A ", 0x1C, 256u << 20},
        {"3GP 3gp4", ".3gp", "Video", "3gp4", 0x18, 2ULL << 30},
        {"3GP 3gp5", ".3gp", "Video", "3gp5", 0x18, 2ULL << 30},
        {"3GP 3ga", ".3ga", "Audio", "3ga ", 0x18, 256u << 20},
        {"MOV qt  ", ".mov", "Video", "qt  ", 0x14, 4ULL << 30},
        {"F4V flash", ".f4v", "Video", "f4v ", 0x18, 4ULL << 30},
        {"CR3 Canon", ".cr3", "Image", "crx ", 0x18, 256u << 20},
    };
    for (const auto& r : rows) {
        FileSignature s;
        s.id = static_cast<int>(signatures.size());
        s.format = r.fmt;
        s.extension = r.ext;
        s.category = r.cat;
        s.maxSize = r.maxS;
        s.header.resize(12);
        s.header[0] = static_cast<uint8_t>((r.boxSize >> 24) & 0xFF);
        s.header[1] = static_cast<uint8_t>((r.boxSize >> 16) & 0xFF);
        s.header[2] = static_cast<uint8_t>((r.boxSize >> 8) & 0xFF);
        s.header[3] = static_cast<uint8_t>(r.boxSize & 0xFF);
        s.header[4] = 'f';
        s.header[5] = 't';
        s.header[6] = 'y';
        s.header[7] = 'p';
        for (int i = 0; i < 4; ++i) s.header[8 + i] = static_cast<uint8_t>(r.brand[i]);
        signatures.push_back(s);
    }
}

// CA-010: absolute signatures dir set by the main process. Packaged apps run
// with a CWD that is NOT the install dir and std::ifstream cannot read inside
// app.asar, so the CWD-relative probes below silently missed in production.
static std::string g_resourceSignatureDir;

void appendResourceSignatureFiles(std::vector<FileSignature>& signatures,
                                  const std::function<std::vector<uint8_t>(const std::string&)>& hexToBytes) {
    static const char* kNames[] = {
        "signatures-extended.json",
        "signatures-supplement.json",
    };
    if (!g_resourceSignatureDir.empty()) {
        for (const char* name : kNames) {
            std::string abs = g_resourceSignatureDir;
            if (!abs.empty() && abs.back() != '/' && abs.back() != '\\') abs += '/';
            abs += name;
            if (!appendSignaturesFromJson(signatures, abs, hexToBytes)) {
                std::cerr << "[byteback] signatures file missing or unparseable: " << abs << std::endl;
            }
        }
        return;
    }
    // Dev fallback: repo-root CWD-relative probes.
    static const char* kFiles[] = {
        // CA-010: resources/signatures.json is NOT auto-loaded — every entry
        // duplicates the embedded set (incl. RIFF triple-reports and a bare
        // MZ that sneaked EXE carving back in). Users pass overlays explicitly
        // via loadSignatures(jsonPath).
        "resources/signatures-extended.json",
        "resources/signatures-supplement.json",
        "../resources/signatures-extended.json",
        "../resources/signatures-supplement.json",
        "../../resources/signatures-extended.json",
        "../../resources/signatures-supplement.json",
        "signatures-extended.json",
        "signatures-supplement.json",
    };
    for (const char* path : kFiles) appendSignaturesFromJson(signatures, path, hexToBytes);
}

} // namespace

void CarvingEngine::setResourceSignatureDir(const std::string& dir) {
    g_resourceSignatureDir = dir;
}

size_t CarvingEngine::globalSignatureCount() {
    static size_t count = 0;
    static std::once_flag once;
    std::call_once(once, [] {
        CarvingEngine engine;
        if (engine.loadSignatures("")) count = engine.signatureCount();
    });
    return count;
}

bool CarvingEngine::loadSignatures(const std::string& jsonPath) {
    signatures.clear();
    // Brand-precise ftyp signatures first: same-offset dedup keeps the first
    // match, so listed brands must precede the generic MP4 entries below.
    appendFtypMediaSignatures(signatures);
    loadEmbeddedSignatures(signatures);

    auto hexFn = [this](const std::string& hex) { return hexToBytes(hex); };
    if (!jsonPath.empty()) appendSignaturesFromJson(signatures, jsonPath, hexFn);
    appendResourceSignatureFiles(signatures, hexFn);

    for (size_t i = 0; i < signatures.size(); ++i) signatures[i].id = static_cast<int>(i);

    buildAhoCorasick();
    return !signatures.empty();
}

void CarvingEngine::buildAhoCorasick() {
    acNodes.clear();
    acNext_.clear();
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

    // CA-023: compile the full transition function. The hot scan loop used to
    // walk fail links with two std::map lookups per non-matching byte
    // (~8M map finds per 4MB chunk); a flat [state][256] table removes that
    // as the scan bottleneck.
    const size_t stateCount = acNodes.size();
    acNext_.assign(stateCount * 256, 0);
    for (int c = 0; c < 256; ++c) {
        auto it = acNodes[0].children.find(static_cast<uint8_t>(c));
        acNext_[c] = (it != acNodes[0].children.end()) ? it->second : 0;
    }
    std::queue<int> order;
    for (auto const& [byte, child] : acNodes[0].children) order.push(child);
    std::vector<bool> visited(stateCount, false);
    while (!order.empty()) {
        const int s = order.front();
        order.pop();
        if (s <= 0 || s >= static_cast<int>(stateCount) || visited[s]) continue;
        visited[s] = true;
        const int fail = acNodes[s].fail;
        for (int c = 0; c < 256; ++c) {
            auto it = acNodes[s].children.find(static_cast<uint8_t>(c));
            const int next = (it != acNodes[s].children.end())
                                 ? it->second
                                 : acNext_[static_cast<size_t>(fail) * 256 + c];
            acNext_[static_cast<size_t>(s) * 256 + c] = next;
            if (it != acNodes[s].children.end()) order.push(it->second);
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
    if (rangeEndSector <= firstSector) return false;

    // CA-006: sequential scan. The old 4-worker banding overlapped bands by
    // only ~2 sectors, so every file straddling a band edge was truncated at
    // the boundary — and all I/O serialized on the reader's mutex anyway, so
    // the parallelism bought nothing. The compiled transition table (below)
    // recovers the throughput instead.
    const bool ok = scanRangeSingle(reader, firstSector, rangeEndSector, callback, isRunning, 0, 0, nullptr);
    FileRecord done;
    done.id = -1;
    done.startSector = rangeEndSector;
    callback(done);
    return ok;
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
        const uint64_t sectorsToRead = std::min<uint64_t>(chunkSectors, rangeEndSector - sector);

        // CA-007: clamp the read to the range end — reads past the disk end
        // fail on physical drives, which silently skipped the last <=4MB of
        // every range. On failure, halve down to single sectors so one bad
        // sector no longer blinds the whole 4MB chunk; the reader's bad-sector
        // telemetry records the failures. Sub-reads are processed in disk
        // order, so automaton state and active carves stay continuous.
        struct SubRead { uint64_t sector; uint32_t sectors; };
        std::vector<SubRead> subReads{{sector, static_cast<uint32_t>(sectorsToRead)}};
        while (!subReads.empty()) {
        const auto sub = subReads.back();
        subReads.pop_back();
        const uint32_t wantBytes = sub.sectors * sectorSize;
        auto res = reader.readSectors(sub.sector * sectorSize, wantBytes, currentBuf->data());
        if ((!res.success || res.bytesRead < wantBytes) && sub.sectors > 1) {
            const uint32_t half = sub.sectors / 2;
            subReads.push_back({sub.sector + half, sub.sectors - half});
            subReads.push_back({sub.sector, half});
            continue;
        }
        if (!res.success) continue;

        uint64_t baseOffset = sub.sector * sectorSize;

        for (uint32_t i = 0; i < res.bytesRead; ++i) {
            uint8_t byte = currentBuf->data()[i];
            currentState = acNext_[static_cast<size_t>(currentState) * 256 + byte];

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
                                auto rres = reader.readBytes(it->startOffset, alignedSize, alignedBuf.data());
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
                                // One shared probe read; the mp4 walk also fetches
                                // box headers beyond it via targeted reads.
                                uint32_t probe = static_cast<uint32_t>(std::min<uint64_t>(actualSize, 1u << 20));
                                probe = ((probe + sectorSize - 1) / sectorSize) * sectorSize;
                                std::vector<uint8_t> probeBuf;
                                if (probe > 0) {
                                    probeBuf.resize(probe);
                                    if (!reader.readBytes(it->startOffset, probe, probeBuf.data()).success) {
                                        probeBuf.clear();
                                    }
                                }
                                if (!probeBuf.empty()) {
                                    if (isMp4FamilyExt(effExt)) {
                                        // CA-018: targeted box walk beats the 1MB probe clamp.
                                        auto readAt = [&reader](uint64_t off, uint32_t len, uint8_t* out) {
                                            auto res = reader.readBytes(off, len, out);
                                            return res.success && res.bytesRead >= len;
                                        };
                                        auto pr = carver::parseIsobmffBounded(probeBuf.data(), probeBuf.size(),
                                                                              it->startOffset, actualSize, readAt);
                                        if (pr.valid) {
                                            if (pr.size > 0 && pr.size <= actualSize) actualSize = pr.size;
                                            if (!pr.extension.empty()) effExt = pr.extension;
                                            if (pr.confidence > confidence) confidence = pr.confidence;
                                        }
                                    } else {
                                        // CA-003: actualSize can exceed the 1MB probe; never
                                        // hand the parser a size larger than the buffer.
                                        applyStructuralRefinement(effExt, probeBuf.data(),
                                                                  static_cast<size_t>(std::min<uint64_t>(actualSize, probeBuf.size())),
                                                                  actualSize, effExt, confidence);
                                    }
                                }
                                auto dot = effName.find_last_of('.');
                                if (dot != std::string::npos) effName = effName.substr(0, dot);
                                effName += std::string(".") + effExt;
                            }
                            if (ext == "riff") {
                                uint8_t hdr[512];
                                uint32_t hdrAligned = ((512u + sectorSize - 1) / sectorSize) * sectorSize;
                                std::vector<uint8_t> hdrBuf(hdrAligned);
                                if (reader.readBytes(it->startOffset, hdrAligned, hdrBuf.data()).success) {
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
                                fr.startByteOffset = it->startOffset % sectorSize;
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
                                stampCarveExif(fr, effExtBgc, reader, it->startOffset, fr.sizeBytes, nullptr, 0);

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
                            fr.startByteOffset = it->startOffset % sectorSize;
                            fr.endSector = (fileEndOffset + sectorSize - 1) / sectorSize;
                            fr.status = 0;
                            fr.confidence = confidence;
                            fr.category = sig.category;
                            fr.source = "carver";
                            fr.createdAt = 0;
                            fr.modifiedAt = 0;
                            stampCarveExif(fr, effExt, reader, it->startOffset, actualSize, nullptr, 0);
                            
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
                int confidence = sig.footer.empty() ? 55 : 70;
                uint32_t probe = static_cast<uint32_t>(std::min<uint64_t>(sig.maxSize, 1u << 20));
                probe = ((probe + sectorSize - 1) / sectorSize) * sectorSize;
                    std::vector<uint8_t> probeBuf;
                    if (probe > 0) {
                        probeBuf.resize(probe);
                        // CA-004: byte-exact read; unaligned candidates were silently
                        // erased here before because readSectors rejected the offset.
                        if (reader.readBytes(it->startOffset, probe, probeBuf.data()).success) {
                            std::string name = it->filename;
                            if (!refineExpiredCarve(sig, &reader, it->startOffset, probeBuf.data(), probeBuf.size(),
                                                    name, effExt, actualSize, confidence)) {
                                it = activeCarves.erase(it);
                                continue;
                            }
                            it->filename = name;
                        } else {
                            it = activeCarves.erase(it);
                            continue;
                        }
                    } else {
                        it = activeCarves.erase(it);
                        continue;
                    }

                FileRecord fr;
                fr.id = 0;
                fr.parentId = 0;
                fr.name = it->filename;
                fr.extension = effExt;
                fr.path = "/recovered_raw/" + fr.name;
                fr.sizeBytes = actualSize;
                fr.startSector = it->startSector;
                fr.startByteOffset = it->startOffset % sectorSize;
                fr.endSector = (it->startOffset + actualSize + sectorSize - 1) / sectorSize;
                fr.status = 0;
                fr.confidence = confidence;
                fr.category = refineCarveCategory(probeBuf.empty() ? nullptr : probeBuf.data(),
                                                  probeBuf.size(), effExt, sig.category);
                fr.source = "carver";
                fr.createdAt = 0;
                fr.modifiedAt = 0;
                stampCarveExif(fr, effExt, reader, it->startOffset, actualSize,
                               probeBuf.empty() ? nullptr : probeBuf.data(), probeBuf.size());
                
                emit(fr);
                
                it = activeCarves.erase(it);
            } else {
                ++it;
            }
        }
        
        FileRecord progressTick;
        progressTick.id = -1;
        progressTick.startSector = sub.sector + sub.sectors;
        emit(progressTick);
        } // sub-reads
    }

    // Process remaining active carves when disk ends
    uint64_t endOfDiskOffset = std::min(diskSize, rangeEndSector * sectorSize);
    for (const auto& ac : activeCarves) {
        const auto& sig = signatures[ac.sigId];
        uint64_t actualSize = std::min(sig.maxSize, endOfDiskOffset > ac.startOffset ? endOfDiskOffset - ac.startOffset : 0);
        std::string effExt = sig.extension.empty() ? "" : sig.extension.substr(1);
        int confidence = sig.footer.empty() ? 55 : 70;
        uint32_t probe = static_cast<uint32_t>(std::min<uint64_t>(actualSize, 1u << 20));
        probe = ((probe + sectorSize - 1) / sectorSize) * sectorSize;
        std::vector<uint8_t> probeBuf;
        if (probe == 0) continue;
        probeBuf.resize(probe);
        if (!reader.readBytes(ac.startOffset, probe, probeBuf.data()).success) continue;

        std::string name = ac.filename;
        if (!refineExpiredCarve(sig, &reader, ac.startOffset, probeBuf.data(), probeBuf.size(),
                                name, effExt, actualSize, confidence)) {
            continue;
        }

        FileRecord fr;
        fr.id = 0;
        fr.parentId = 0;
        fr.name = name;
        fr.extension = effExt;
        fr.path = "/recovered_raw/" + fr.name;
        fr.sizeBytes = actualSize;
        fr.startSector = ac.startSector;
        fr.startByteOffset = ac.startOffset % sectorSize;
        fr.endSector = (ac.startOffset + actualSize + sectorSize - 1) / sectorSize;
        fr.status = 0;
        fr.confidence = confidence;
        fr.category = refineCarveCategory(probeBuf.data(), probeBuf.size(), effExt, sig.category);
        fr.source = "carver";
        fr.createdAt = 0;
        fr.modifiedAt = 0;
        stampCarveExif(fr, effExt, reader, ac.startOffset, actualSize, probeBuf.data(), probeBuf.size());
        emit(fr);
    }

    return true;
}

} // namespace byteback
