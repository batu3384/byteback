#ifndef WOLF_SHREDDER_H
#define WOLF_SHREDDER_H

#include <string>
#include <cstdint>

namespace security {

class DataShredder {
public:
    DataShredder() = default;
    ~DataShredder() = default;

    // Shreds a file using DoD 5220.22-M standard (3 passes)
    // Pass 1: 0x00, Pass 2: 0xFF, Pass 3: Random
    bool shred_file(const std::string& file_path);

private:
    bool overwrite_pass(const std::string& file_path, std::size_t file_size, uint8_t pattern, bool is_random);
    std::size_t get_file_size(const std::string& file_path);
};

} // namespace security

#endif // WOLF_SHREDDER_H
