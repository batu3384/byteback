#include "wolf_carver.h"
#include "wolf_memory.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <future>
#include <algorithm>

namespace wolf {

CarvingEngine::CarvingEngine() {}
CarvingEngine::~CarvingEngine() {}

std::vector<uint8_t> CarvingEngine::hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = (uint8_t)strtol(byteString.c_str(), nullptr, 16);
        bytes.push_back(byte);
    }
    return bytes;
}

bool CarvingEngine::loadSignatures(const std::string& /*jsonPath*/) {
    // We embed the signatures directly for maximum reliability.
    signatures.clear();

    auto addSig = [&](const std::string& fmt, const std::string& ext, const std::string& cat, 
                      const std::vector<uint8_t>& head, const std::vector<uint8_t>& foot, uint64_t maxS) {
        FileSignature s;
        s.format = fmt; s.extension = ext; s.category = cat;
        s.header = head; s.footer = foot; s.maxSize = maxS;
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
    addSig("WebP Image", ".webp", "Image", {0x52, 0x49, 0x46, 0x46}, {}, 30 * 1024 * 1024); // RIFF header, WebP subtype
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
    addSig("OpenDocument Text", ".odt", "Document", {0x50, 0x4B, 0x03, 0x04}, {}, 50 * 1024 * 1024);
    addSig("EPUB eBook", ".epub", "Document", {0x50, 0x4B, 0x03, 0x04}, {}, 50 * 1024 * 1024);
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
    addSig("AVI Video", ".avi", "Video", {0x52, 0x49, 0x46, 0x46}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("MKV Video", ".mkv", "Video", {0x1A, 0x45, 0xDF, 0xA3}, {}, 4ULL * 1024 * 1024 * 1024);
    addSig("MPEG Video", ".mpg", "Video", {0x00, 0x00, 0x01, 0xBA}, {0x00, 0x00, 0x01, 0xB9}, 2ULL * 1024 * 1024 * 1024);
    addSig("MOV Video", ".mov", "Video", {0x00, 0x00, 0x00, 0x14, 0x66, 0x74, 0x79, 0x70, 0x71, 0x74}, {}, 4ULL * 1024 * 1024 * 1024);
    addSig("FLV Video", ".flv", "Video", {0x46, 0x4C, 0x56, 0x01}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("WMV Video", ".wmv", "Video", {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11}, {}, 2ULL * 1024 * 1024 * 1024);
    addSig("3GP Video", ".3gp", "Video", {0x00, 0x00, 0x00, 0x14, 0x66, 0x74, 0x79, 0x70, 0x33, 0x67, 0x70}, {}, 1ULL * 1024 * 1024 * 1024);
    addSig("WebM Video", ".webm", "Video", {0x1A, 0x45, 0xDF, 0xA3}, {}, 4ULL * 1024 * 1024 * 1024);
    addSig("MPEG-TS", ".ts", "Video", {0x47}, {}, 4ULL * 1024 * 1024 * 1024);
    addSig("M4V Video", ".m4v", "Video", {0x00, 0x00, 0x00, 0x18, 0x66, 0x74, 0x79, 0x70, 0x4D, 0x34, 0x56}, {}, 4ULL * 1024 * 1024 * 1024);
    addSig("SWF Flash", ".swf", "Video", {0x46, 0x57, 0x53}, {}, 100 * 1024 * 1024);

    // ============================================================
    // Audio (12 signatures)
    // ============================================================
    addSig("MP3 Audio", ".mp3", "Audio", {0x49, 0x44, 0x33}, {}, 20 * 1024 * 1024);
    addSig("MP3 Audio (no ID3)", ".mp3", "Audio", {0xFF, 0xFB}, {}, 20 * 1024 * 1024);
    addSig("FLAC Audio", ".flac", "Audio", {0x66, 0x4C, 0x61, 0x43}, {}, 100 * 1024 * 1024);
    addSig("WAV Audio", ".wav", "Audio", {0x52, 0x49, 0x46, 0x46}, {}, 100 * 1024 * 1024);
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
    // Executables & System (8 signatures)
    // ============================================================
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

    return true;
}

bool CarvingEngine::scan(DiskReader& reader, FileSystemParser::FileRecordCallback callback, std::atomic<bool>* isRunning) {
    if (!reader.isOpen() || signatures.empty()) return false;

    uint64_t diskSize = reader.getDiskSize();
    uint32_t sectorSize = reader.getSectorSize();
    if (sectorSize == 0) sectorSize = 512;
    
    const uint32_t chunkSectors = 8192; // 4MB chunks
    const uint32_t chunkSize = chunkSectors * sectorSize;
    auto poolBufA = MemoryPool::getInstance().acquireBuffer(chunkSize);
    auto* currentBuf = poolBufA.get();
    
    uint64_t maxSector = diskSize / sectorSize;
    int foundCount = 0;

    for (uint64_t sector = 0; sector < maxSector; sector += chunkSectors) {
        if (isRunning && !(*isRunning)) break;
        
        auto res = reader.readSectors(sector * sectorSize, chunkSize, currentBuf->data());
        if (!res.success) continue;

        for (uint32_t i = 0; i < res.bytesRead; i += sectorSize) {
            for (const auto& sig : signatures) {
                if (sig.header.empty() || i + sig.header.size() > res.bytesRead) continue;
                
                bool match = true;
                for (size_t h = 0; h < sig.header.size(); ++h) {
                    if (currentBuf->data()[i + h] != sig.header[h]) {
                        match = false;
                        break;
                    }
                }
                
                if (match) {
                    double entropy = EntropyAnalyzer::calculateShannonEntropy(currentBuf->data(), res.bytesRead, i, std::min<uint32_t>((uint32_t)4096, (uint32_t)(res.bytesRead - i)));
                    if (entropy < 1.0 && sig.category == "Archive") continue; 

                    uint64_t actualSize = sig.maxSize;
                    bool footerFound = false;

                    if (!sig.footer.empty()) {
                        // 1. Search in current chunk
                        if (i + sig.header.size() < res.bytesRead) {
                            uint32_t maxSearchJ = res.bytesRead >= sig.footer.size() ? res.bytesRead - (uint32_t)sig.footer.size() : 0;
                            for (uint32_t j = i + (uint32_t)sig.header.size(); j <= maxSearchJ; ++j) {
                                bool fMatch = true;
                                for (size_t f = 0; f < sig.footer.size(); ++f) {
                                    if (currentBuf->data()[j + f] != sig.footer[f]) {
                                        fMatch = false; break;
                                    }
                                }
                                if (fMatch) {
                                    actualSize = (j + sig.footer.size()) - i;
                                    footerFound = true;
                                    break;
                                }
                            }
                        }

                        // 2. Search ahead if not found
                        if (!footerFound) {
                            uint64_t maxSearchBytes = std::min(sig.maxSize, diskSize - (sector * sectorSize + i));
                            uint64_t bytesSearched = res.bytesRead - i;
                            uint64_t currentOffset = sector * sectorSize + res.bytesRead;

                            auto tempPoolBuf = MemoryPool::getInstance().acquireBuffer(chunkSize);
                            auto* tempBuf = tempPoolBuf.get();

                            while (bytesSearched < maxSearchBytes) {
                                if (isRunning && !(*isRunning)) break;

                                uint32_t readSize = (uint32_t)std::min((uint64_t)chunkSize, maxSearchBytes - bytesSearched);
                                readSize = ((readSize + sectorSize - 1) / sectorSize) * sectorSize; // Align to sector
                                if (readSize == 0 || currentOffset + readSize > diskSize) break;

                                auto tempRes = reader.readSectors(currentOffset, readSize, tempBuf->data());
                                if (!tempRes.success || tempRes.bytesRead == 0) break;

                                uint32_t maxSearchJ = tempRes.bytesRead >= sig.footer.size() ? tempRes.bytesRead - (uint32_t)sig.footer.size() : 0;
                                for (uint32_t j = 0; j <= maxSearchJ; ++j) {
                                    bool fMatch = true;
                                    for (size_t f = 0; f < sig.footer.size(); ++f) {
                                        if (tempBuf->data()[j + f] != sig.footer[f]) {
                                            fMatch = false; break;
                                        }
                                    }
                                    if (fMatch) {
                                        actualSize = bytesSearched + j + sig.footer.size();
                                        footerFound = true;
                                        break;
                                    }
                                }

                                if (footerFound) break;

                                bytesSearched += tempRes.bytesRead;
                                currentOffset += tempRes.bytesRead;
                            }
                        }
                    }

                    FileRecord fr;
                    fr.id = 0;
                    fr.parentId = 0;
                    uint64_t startSec = sector + (i / sectorSize);
                    fr.name = "carved_" + std::to_string(foundCount++) + "_" + std::to_string(startSec) + sig.extension;
                    fr.extension = sig.extension.empty() ? "" : sig.extension.substr(1);
                    fr.path = "/recovered_raw/" + fr.name;
                    fr.sizeBytes = actualSize; 
                    fr.startSector = startSec;
                    uint64_t endSectorsOff = (actualSize + sectorSize - 1) / sectorSize;
                    fr.endSector = fr.startSector + endSectorsOff;
                    fr.status = 0;
                    fr.confidence = footerFound ? 95 : 70;
                    fr.category = sig.category;
                    fr.source = "carver";
                    fr.createdAt = 0;
                    fr.modifiedAt = 0;
                    
                    callback(fr);
                }
            }
        }
        
        FileRecord progressTick;
        progressTick.id = -1;
        progressTick.startSector = sector + chunkSectors;
        callback(progressTick);
    }

    return true;
}
} // namespace wolf
