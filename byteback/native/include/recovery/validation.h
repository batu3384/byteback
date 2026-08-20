#pragma once

#include "byteback_recovery.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace byteback {

std::string extensionFromRecord(const FileRecord& record);

int validateCarvedBuffer(const std::string& ext, const uint8_t* data, size_t size);

void applyPostRecoveryValidation(RecoveryResult& result, const FileRecord& record);

} // namespace byteback
