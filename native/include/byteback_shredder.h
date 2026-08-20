#ifndef BYTEBACK_SHREDDER_H
#define BYTEBACK_SHREDDER_H

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

    // PhysicalDrive wipe. typedSerial must match actualSerial (case-insensitive).
    // Tests never pass a live drive; mismatch/empty always false.
    static bool wipeSerialMatches(const std::string& typed, const std::string& actual);
    bool shred_physical_drive(int driveIndex, const std::string& typedSerial,
                              const std::string& actualSerial, uint64_t sizeBytes);

private:
    bool overwrite_pass(const std::string& file_path, std::size_t file_size, uint8_t pattern, bool is_random);
    std::size_t get_file_size(const std::string& file_path);
};

} // namespace security

#endif // BYTEBACK_SHREDDER_H
