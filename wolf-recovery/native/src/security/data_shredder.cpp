#include "../../include/wolf_shredder.h"

#include <windows.h>
#include <iostream>
#include <random>
#include <vector>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace security {

namespace {
std::string normSerial(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isspace(c)) continue;
        o.push_back(static_cast<char>(std::toupper(c)));
    }
    return o;
}
}

bool DataShredder::wipeSerialMatches(const std::string& typed, const std::string& actual) {
    if (typed.empty() || actual.empty()) return false;
    return normSerial(typed) == normSerial(actual);
}

bool DataShredder::shred_physical_drive(int driveIndex, const std::string& typedSerial,
                                        const std::string& actualSerial, uint64_t sizeBytes) {
    if (driveIndex < 0 || sizeBytes < 512) return false;
    if (!wipeSerialMatches(typedSerial, actualSerial)) return false;
#ifdef _WIN32
    char path[64];
    snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", driveIndex);
    const std::size_t n = static_cast<std::size_t>(sizeBytes);
    if (!overwrite_pass(path, n, 0x00, false)) return false;
    if (!overwrite_pass(path, n, 0xFF, false)) return false;
    return overwrite_pass(path, n, 0x00, true);
#else
    (void)driveIndex;
    return false;
#endif
}

bool DataShredder::overwrite_pass(const std::string& file_path, std::size_t file_size, uint8_t pattern, bool is_random) {
    HANDLE hFile = CreateFileA(
        file_path.c_str(),
        GENERIC_WRITE,
        0, // Exclusive access
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_WRITE_THROUGH, // Write directly to disk bypassing cache if possible
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    const DWORD buffer_size = 65536; // 64 KB buffer
    std::vector<uint8_t> buffer(buffer_size);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint16_t> dis(0, 255);

    std::size_t bytes_written_total = 0;
    while (bytes_written_total < file_size) {
        DWORD to_write = static_cast<DWORD>(std::min<std::size_t>(buffer_size, file_size - bytes_written_total));

        if (is_random) {
            for (DWORD i = 0; i < to_write; ++i) {
                buffer[i] = static_cast<uint8_t>(dis(gen));
            }
        } else {
            std::fill(buffer.begin(), buffer.begin() + to_write, pattern);
        }

        LARGE_INTEGER li;
        li.QuadPart = bytes_written_total;
        SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);

        DWORD bytes_written_chunk = 0;
        if (!WriteFile(hFile, buffer.data(), to_write, &bytes_written_chunk, NULL)) {
            CloseHandle(hFile);
            return false;
        }

        if (bytes_written_chunk != to_write) {
            CloseHandle(hFile);
            return false;
        }

        bytes_written_total += bytes_written_chunk;
    }

    FlushFileBuffers(hFile);
    CloseHandle(hFile);
    return true;
}

std::size_t DataShredder::get_file_size(const std::string& file_path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(file_path, ec);
    if (ec) {
        return 0;
    }
    return size;
}

bool DataShredder::shred_file(const std::string& file_path) {
    std::size_t size = get_file_size(file_path);
    if (size == 0) {
        std::error_code ec;
        if (std::filesystem::exists(file_path, ec)) {
            return std::filesystem::remove(file_path, ec);
        }
        return false;
    }

    if (!overwrite_pass(file_path, size, 0x00, false)) {
        return false;
    }

    if (!overwrite_pass(file_path, size, 0xFF, false)) {
        return false;
    }

    if (!overwrite_pass(file_path, size, 0x00, true)) {
        return false;
    }

    std::error_code ec;
    return std::filesystem::remove(file_path, ec);
}

bool DataShredder::shred_free_space(const std::string& dirPath, uint64_t maxFillBytes) {
    if (dirPath.empty()) return false;
    if (dirPath.rfind("\\\\.\\", 0) == 0 || dirPath.rfind("\\\\?\\", 0) == 0) return false;
    if (dirPath.find("PhysicalDrive") != std::string::npos) return false;

    std::error_code ec;
    if (!std::filesystem::is_directory(dirPath, ec)) return false;

    const auto filler = (std::filesystem::path(dirPath) / ".wolf_freespace_wipe.tmp").string();
    {
        std::ofstream out(filler, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        const size_t bufSize = 65536;
        std::vector<uint8_t> buf(bufSize, 0);
        uint64_t written = 0;
        while (maxFillBytes == 0 || written < maxFillBytes) {
            size_t take = bufSize;
            if (maxFillBytes > 0 && written + take > maxFillBytes) {
                take = static_cast<size_t>(maxFillBytes - written);
            }
            out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(take));
            if (!out) break;
            written += take;
            if (take < bufSize) break;
        }
        out.close();
        if (written == 0) {
            std::filesystem::remove(filler, ec);
            return false;
        }
    }
    return shred_file(filler);
}

} // namespace security
