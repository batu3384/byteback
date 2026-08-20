#ifndef WOLF_SHREDDER_H
#define WOLF_SHREDDER_H

#include <string>
#include <cstdint>

namespace security {

class DataShredder {
public:
    DataShredder() = default;
    ~DataShredder() = default;

    // DoD 5220.22-M 3-pass file shred (0x00, 0xFF, random) then unlink.
    bool shred_file(const std::string& file_path);

    // NIST SP 800-88 free-space wipe via filler file in dirPath, then DoD shred.
    // maxFillBytes==0 fills until the volume is full. Device paths are refused.
    bool shred_free_space(const std::string& dirPath, uint64_t maxFillBytes = 0);

private:
    bool overwrite_pass(const std::string& file_path, std::size_t file_size, uint8_t pattern, bool is_random);
    std::size_t get_file_size(const std::string& file_path);
};

} // namespace security

#endif // WOLF_SHREDDER_H
