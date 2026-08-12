#include "../../include/wolf_shredder.h"

#include <windows.h>
#include <iostream>
#include <random>
#include <vector>
#include <filesystem>
#include <algorithm>

namespace security {

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

} // namespace security
