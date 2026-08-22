#include "recovery/preview_reader.h"
#include "byteback_memory.h"
#include "carver/file_validators.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#ifdef _WIN32
#include <process.h>
#endif

namespace byteback {

namespace {

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t readBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

struct BitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t bitPos = 0;

    bool getBit() {
        if (bitPos >= size * 8) return false;
        const size_t byteIdx = bitPos / 8;
        const int bitIdx = 7 - static_cast<int>(bitPos % 8);
        ++bitPos;
        return ((data[byteIdx] >> bitIdx) & 1) != 0;
    }

    uint32_t readUe() {
        int zeros = 0;
        while (!getBit() && zeros < 31) ++zeros;
        if (zeros == 0) return 0;
        uint32_t val = 1;
        for (int i = 0; i < zeros; ++i) val = (val << 1) | (getBit() ? 1u : 0u);
        return val - 1;
    }
};

bool looksLikeBmp(const uint8_t* data, size_t size) {
    if (size < 14 || data[0] != 'B' || data[1] != 'M') return false;
    const uint32_t bfSize = readLe32(data + 2);
    const uint32_t bfOffBits = readLe32(data + 10);
    if (bfOffBits < 14 || bfOffBits > 1024 * 1024) return false;
    if (bfSize > 0 && bfSize < bfOffBits) return false;
    if (size >= 18) {
        const uint32_t dib = readLe32(data + 14);
        if (dib != 12 && dib != 40 && dib != 108 && dib != 124) return false;
    }
    return true;
}

std::string sniffMime(const uint8_t* data, size_t size) {
    if (!data || size < 3) return {};
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return "image/jpeg";
    if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G' &&
        data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A) {
        return "image/png";
    }
    if (size >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8' &&
        (data[4] == '7' || data[4] == '9') && data[5] == 'a') {
        return "image/gif";
    }
    if (size >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
        return "image/webp";
    }
    if (looksLikeBmp(data, size)) return "image/bmp";
    if (size >= 4 && data[0] == '%' && data[1] == 'P' && data[2] == 'D' && data[3] == 'F') {
        return "application/pdf";
    }
    return {};
}

std::string extensionOf(const FileRecord& record) {
    auto dot = record.name.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = record.name.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

bool looksLikeBinaryContainer(const uint8_t* data, size_t size) {
    if (!data || size < 4) return false;
    if (size >= 8 && std::memcmp(data + 4, "ftyp", 4) == 0) return true;
    if (std::memcmp(data, "RIFF", 4) == 0) return true;
    if (data[0] == 0x1a && data[1] == 0x45 && data[2] == 0xdf && data[3] == 0xa3) return true;
    if (data[0] == 'P' && data[1] == 'K') return true;
    if (data[0] == 'M' && data[1] == 'Z') return true;
    return false;
}

std::string kindFromMime(const std::string& mime) {
    if (mime.rfind("image/", 0) == 0) return "image";
    if (mime == "application/pdf") return "pdf";
    return "binary";
}

std::string kindFromExtension(const std::string& ext) {
    if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "webp" ||
        ext == "bmp" || ext == "ico" || ext == "tif" || ext == "tiff") {
        return "image";
    }
    if (ext == "pdf") return "pdf";
    if (ext == "txt" || ext == "log" || ext == "csv" || ext == "json" || ext == "xml" ||
        ext == "md" || ext == "html" || ext == "htm") {
        return "text";
    }
    return {};
}

void classifyPreview(const FileRecord& record, const uint8_t* data, size_t size,
                     std::string& kind, std::string& mime) {
    kind.clear();
    mime.clear();

    mime = sniffMime(data, size);
    if (!mime.empty()) {
        kind = kindFromMime(mime);
        return;
    }

    const std::string ext = extensionOf(record);
    // Extension claims BMP but magic failed — do not pretend it is an image.
    if (ext == "bmp") {
        kind = "binary";
        return;
    }

    kind = kindFromExtension(ext);
    if (!kind.empty()) {
        if (kind == "image") {
            // Extension-only: name a real MIME or refuse image claim (no fake image/png).
            if (ext == "jpg" || ext == "jpeg") mime = "image/jpeg";
            else if (ext == "png") mime = "image/png";
            else if (ext == "gif") mime = "image/gif";
            else if (ext == "webp") mime = "image/webp";
            else if (ext == "ico") mime = "image/x-icon";
            else {
                kind = "binary";
                mime.clear();
            }
        }
        return;
    }

    if (size > 0) {
        if (looksLikeBinaryContainer(data, size)) {
            kind = "binary";
            return;
        }
        size_t printable = 0;
        size_t sample = std::min(size, size_t{512});
        for (size_t i = 0; i < sample; ++i) {
            unsigned char c = data[i];
            if (c == 9 || c == 10 || c == 13 || (c >= 32 && c < 127)) ++printable;
        }
        if (printable * 100 / sample >= 85) {
            kind = "text";
            mime = "text/plain";
            return;
        }
    }
    kind = "binary";
}

bool isVideoRecord(const FileRecord& record) {
    if (record.category == "Video") return true;
    const std::string ext = extensionOf(record);
    return ext == "avi" || ext == "mp4" || ext == "mov" || ext == "mkv" || ext == "webm" ||
           ext == "mpg" || ext == "mpeg" || ext == "m4v" || ext == "3gp" || ext == "ts";
}

// ponytail: MJPEG-in-AVI / MP4 covr / mdat IDR sniff; no H.264 decode.
bool findEmbeddedJpeg(const uint8_t* data, size_t size, size_t& outOff, size_t& outLen) {
    if (!data || size < 4) return false;
    const size_t limit = std::min(size, size_t{512 * 1024});
    for (size_t i = 0; i + 4 < limit; ++i) {
        if (data[i] != 0xFF || data[i + 1] != 0xD8 || data[i + 2] != 0xFF) continue;
        for (size_t j = i + 4; j + 1 < limit; ++j) {
            if (data[j] == 0xFF && data[j + 1] == 0xD9) {
                const size_t len = j + 2 - i;
                if (byteback::carver::validateJpeg(data + i, len) >= 50) {
                    outOff = i;
                    outLen = len;
                    return true;
                }
                break;
            }
        }
    }
    return false;
}

bool isMp4LikePrefix(const uint8_t* data, size_t size) {
    return size >= 12 && std::memcmp(data + 4, "ftyp", 4) == 0;
}

bool isMkvLikePrefix(const uint8_t* data, size_t size) {
    return size >= 4 && data[0] == 0x1a && data[1] == 0x45 && data[2] == 0xdf && data[3] == 0xa3;
}

bool applyEmbeddedJpegSlice(FilePreviewResult& out, const std::vector<uint8_t>& buf, size_t off, size_t len) {
    if (off + len > buf.size()) return false;
    out.data.assign(buf.begin() + static_cast<std::ptrdiff_t>(off),
                    buf.begin() + static_cast<std::ptrdiff_t>(off + len));
    out.kind = "image";
    out.mime = "image/jpeg";
    return true;
}

bool tryImagePayload(const uint8_t* payload, size_t len, std::vector<uint8_t>& imgOut, std::string& mimeOut) {
    if (!payload || len < 4) return false;
    for (size_t skip : {0u, 8u, 12u, 16u}) {
        if (skip + 4 >= len) continue;
        const uint8_t* p = payload + skip;
        const size_t n = len - skip;
        if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) {
            size_t off = 0;
            size_t jlen = 0;
            if (!findEmbeddedJpeg(p, n, off, jlen)) continue;
            imgOut.assign(p + off, p + off + jlen);
            mimeOut = "image/jpeg";
            return true;
        }
        if (n >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') {
            imgOut.assign(p, p + n);
            mimeOut = "image/png";
            return true;
        }
    }
    return false;
}

bool extractMp4CoverImage(const uint8_t* data, size_t size, std::vector<uint8_t>& imgOut, std::string& mimeOut) {
    size_t pos = 0;
    while (pos + 8 <= size) {
        uint64_t atomSize = readBe32(data + pos);
        const char* type = reinterpret_cast<const char*>(data + pos + 4);
        uint64_t header = 8;
        if (atomSize == 1) {
            if (pos + 16 > size) break;
            atomSize = 0;
            for (int i = 0; i < 8; ++i) atomSize = (atomSize << 8) | data[pos + 8 + i];
            header = 16;
        } else if (atomSize == 0) {
            atomSize = size - pos;
        }
        if (atomSize < header || pos + atomSize > size) break;
        const uint8_t* payload = data + pos + static_cast<size_t>(header);
        const size_t payloadLen = static_cast<size_t>(atomSize - header);

        if (std::memcmp(type, "data", 4) == 0 || std::memcmp(type, "covr", 4) == 0 ||
            std::memcmp(type, "thmb", 4) == 0) {
            if (tryImagePayload(payload, payloadLen, imgOut, mimeOut)) return true;
        }

        const bool container = std::memcmp(type, "moov", 4) == 0 || std::memcmp(type, "udta", 4) == 0 ||
                               std::memcmp(type, "meta", 4) == 0 || std::memcmp(type, "ilst", 4) == 0 ||
                               std::memcmp(type, "trak", 4) == 0;
        if (container && payloadLen >= 8) {
            if (extractMp4CoverImage(payload, payloadLen, imgOut, mimeOut)) return true;
        }

        if (atomSize == 0) break;
        pos += static_cast<size_t>(atomSize);
    }
    return false;
}

bool parseSpsDimensions(const uint8_t* nal, size_t nalLen, uint32_t& width, uint32_t& height) {
    if (!nal || nalLen < 8 || (nal[0] & 0x1F) != 7) return false;
    const uint8_t profileIdc = nal[1];
    BitReader br{nal + 1, nalLen - 1};
    br.bitPos = 24; // profile + constraint + level
    br.readUe(); // seq_parameter_set_id
    if (profileIdc == 100 || profileIdc == 110 || profileIdc == 122 || profileIdc == 244 ||
        profileIdc == 44 || profileIdc == 83 || profileIdc == 86 || profileIdc == 118 ||
        profileIdc == 128 || profileIdc == 138 || profileIdc == 139 || profileIdc == 134 ||
        profileIdc == 135) {
        const uint32_t chroma = br.readUe();
        if (chroma == 3) br.getBit();
        br.readUe();
        br.readUe();
        br.getBit();
        if (br.getBit()) {
            const int n = chroma != 3 ? 8 : 12;
            for (int i = 0; i < n; ++i) {
                if (!br.getBit()) continue;
                int last = 8;
                int next = 8;
                const int cnt = i < 6 ? 16 : 64;
                for (int j = 0; j < cnt; ++j) {
                    if (next) {
                        const int delta = static_cast<int>(br.readUe());
                        next = (last + delta + 256) % 256;
                    }
                    last = next ? next : last;
                }
            }
        }
    }
    br.readUe(); // log2_max_frame_num_minus4
    const uint32_t poc = br.readUe();
    if (poc == 0) {
        br.readUe();
    } else if (poc == 1) {
        br.getBit();
        const uint32_t cycles = br.readUe();
        for (uint32_t i = 0; i <= cycles; ++i) br.readUe();
    }
    br.readUe(); // max_num_ref_frames
    br.getBit(); // gaps_in_frame_num_allowed_flag
    const uint32_t wMbs = br.readUe() + 1;
    const uint32_t hMap = br.readUe() + 1;
    const bool frameMbsOnly = br.getBit();
    width = wMbs * 16;
    height = (2 - (frameMbsOnly ? 1u : 0u)) * hMap * 16;
    return width > 0 && height > 0;
}

struct Mp4AvcHint {
    bool hasIdr = false;
    bool hasSps = false;
    uint64_t idrOff = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

bool sniffMp4MdatAvc(const uint8_t* data, size_t size, Mp4AvcHint& hint) {
    size_t pos = 0;
    while (pos + 8 <= size) {
        uint64_t atomSize = readBe32(data + pos);
        const char* type = reinterpret_cast<const char*>(data + pos + 4);
        uint64_t header = 8;
        if (atomSize == 1) {
            if (pos + 16 > size) break;
            atomSize = 0;
            for (int i = 0; i < 8; ++i) atomSize = (atomSize << 8) | data[pos + 8 + i];
            header = 16;
        } else if (atomSize == 0) {
            atomSize = size - pos;
        }
        if (atomSize < header || pos + atomSize > size) break;

        if (std::memcmp(type, "mdat", 4) == 0) {
            const uint8_t* mdat = data + pos + static_cast<size_t>(header);
            const size_t mlen = static_cast<size_t>(atomSize - header);
            const uint8_t* lastSps = nullptr;
            size_t lastSpsLen = 0;
            size_t i = 0;
            while (i + 4 < mlen) {
                const uint32_t n = readBe32(mdat + i);
                i += 4;
                if (n == 0 || i + n > mlen) break;
                if (n >= 1) {
                    const uint8_t nalType = mdat[i] & 0x1F;
                    if (nalType == 7 && n >= 4) {
                        lastSps = mdat + i;
                        lastSpsLen = n;
                        hint.hasSps = true;
                        parseSpsDimensions(lastSps, lastSpsLen, hint.width, hint.height);
                    } else if (nalType == 5) {
                        hint.hasIdr = true;
                        hint.idrOff = pos + header + i;
                        if (lastSps && hint.width == 0) {
                            parseSpsDimensions(lastSps, lastSpsLen, hint.width, hint.height);
                        }
                        return true;
                    }
                }
                i += n;
            }
            return hint.hasSps;
        }

        if (atomSize == 0) break;
        pos += static_cast<size_t>(atomSize);
    }
    return false;
}

std::string formatMp4AvcNote(const Mp4AvcHint& hint) {
    if (!hint.hasIdr && !hint.hasSps) return {};
    std::string note = "H.264";
    if (hint.hasIdr) note += " · IDR kare @ +" + std::to_string(hint.idrOff);
    if (hint.width > 0 && hint.height > 0) {
        note += " · " + std::to_string(hint.width) + "×" + std::to_string(hint.height) + " (SPS)";
    } else if (hint.hasSps) {
        note += " · SPS bulundu";
    }
    note += " · decode yok";
    return note;
}

void tryVideoFramePreview(DiskReader& reader, const FileRecord& record, FilePreviewResult& out) {
    if (out.data.empty() || !isVideoRecord(record) || out.kind == "image") return;

    const std::string ext = extensionOf(record);

    if (isMp4LikePrefix(out.data.data(), out.data.size())) {
        std::vector<uint8_t> cover;
        std::string coverMime;
        if (extractMp4CoverImage(out.data.data(), out.data.size(), cover, coverMime)) {
            out.data = std::move(cover);
            out.kind = "image";
            out.mime = coverMime;
            return;
        }

        Mp4AvcHint hint;
        if (sniffMp4MdatAvc(out.data.data(), out.data.size(), hint)) {
            out.note = formatMp4AvcNote(hint);
        }
    }

    if (isMkvLikePrefix(out.data.data(), out.data.size()) || ext == "mkv" || ext == "webm") {
        std::vector<uint8_t> head;
        if (readRecordPrefix(reader, record, head, kVideoHeadMaxBytes)) {
            size_t off = 0;
            size_t len = 0;
            if (findEmbeddedJpeg(head.data(), head.size(), off, len) &&
                applyEmbeddedJpegSlice(out, head, off, len)) {
                return;
            }
        }
    }

    size_t off = 0;
    size_t len = 0;
    if (!findEmbeddedJpeg(out.data.data(), out.data.size(), off, len)) return;
    applyEmbeddedJpegSlice(out, out.data, off, len);
}

#ifdef _WIN32
// ponytail: optional PATH/bundled ffmpeg via BYTEBACK_FFMPEG; no link-time dep.
namespace {

struct ScopedRemove {
    std::vector<std::filesystem::path> paths;
    ~ScopedRemove() {
        for (const auto& p : paths) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    }
};

std::string previewTempToken(const FileRecord& record) {
    static std::atomic<uint32_t> seq{0};
    const uint32_t n = seq.fetch_add(1, std::memory_order_relaxed);
    const int pid = _getpid();
    return std::to_string(record.id) + "_" + std::to_string(pid) + "_" + std::to_string(n);
}

std::string resolveFfmpegExe() {
    if (const char* env = std::getenv("BYTEBACK_FFMPEG")) {
        if (env[0] != '\0') return std::string(env);
    }
    return "ffmpeg";
}

bool ffmpegReachable(const std::string& exe) {
    namespace fs = std::filesystem;
    if (exe.find('/') != std::string::npos || exe.find('\\') != std::string::npos) {
        std::error_code ec;
        return fs::exists(exe, ec);
    }
    const std::string cmd = "where \"" + exe + "\" >nul 2>nul";
    return std::system(cmd.c_str()) == 0;
}

void setVideoPreviewNote(FilePreviewResult& out, const char* msg) {
    if (out.note.empty()) out.note = msg;
}

} // namespace

bool tryFfmpegVideoFrame(DiskReader& reader, const FileRecord& record, FilePreviewResult& out) {
    if (out.kind == "image" || !isVideoRecord(record)) return false;

    const std::string ffmpeg = resolveFfmpegExe();
    if (!ffmpegReachable(ffmpeg)) {
        setVideoPreviewNote(out, "Video · FFmpeg bulunamadı (PATH veya BYTEBACK_FFMPEG)");
        return false;
    }

    std::vector<uint8_t> buf;
    if (!readRecordPrefix(reader, record, buf, kFfmpegProbeMaxBytes) || buf.size() < 32) {
        setVideoPreviewNote(out, "Video · okuma başarısız");
        return false;
    }

    const std::string ext = extensionOf(record);
    std::string suffix = ".mp4";
    if (ext == "mkv" || ext == "webm") suffix = ".mkv";
    else if (ext == "avi") suffix = ".avi";
    else if (ext == "mov") suffix = ".mov";
    else if (ext == "mpg" || ext == "mpeg") suffix = ".mpg";

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path();
    const std::string token = previewTempToken(record);
    const fs::path inPath = dir / ("bbprev_in_" + token + suffix);
    const fs::path outPath = dir / ("bbprev_out_" + token + ".jpg");

    ScopedRemove cleanup;
    cleanup.paths = {inPath, outPath};

    {
        std::ofstream ofs(inPath, std::ios::binary);
        if (!ofs) {
            setVideoPreviewNote(out, "Video · geçici dosya yazılamadı");
            return false;
        }
        ofs.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    }

    const std::string cmd = "\"" + ffmpeg + "\" -hide_banner -loglevel error -y -i \"" + inPath.string() +
                            "\" -frames:v 1 -q:v 3 \"" + outPath.string() + "\" 2>nul";
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        setVideoPreviewNote(out, "Video · FFmpeg ilk kare başarısız");
        return false;
    }

    std::ifstream ifs(outPath, std::ios::binary);
    if (!ifs) {
        setVideoPreviewNote(out, "Video · FFmpeg çıktısı okunamadı");
        return false;
    }
    std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (jpeg.size() < 4 || byteback::carver::validateJpeg(jpeg.data(), jpeg.size()) < 50) {
        setVideoPreviewNote(out, "Video · FFmpeg geçersiz JPEG üretti");
        return false;
    }

    out.data = std::move(jpeg);
    out.kind = "image";
    out.mime = "image/jpeg";
    out.note = "FFmpeg ilk kare";
    return true;
}
#endif

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
    classifyPreview(record, out.data.data(), out.data.size(), out.kind, out.mime);
    tryVideoFramePreview(reader, record, out);
#ifdef _WIN32
    if (out.kind != "image") tryFfmpegVideoFrame(reader, record, out);
#else
    if (out.kind != "image" && isVideoRecord(record) && out.note.empty()) {
        out.note = "Video · FFmpeg önizleme yalnızca Windows'ta";
    }
#endif
    return out;
}

} // namespace byteback
