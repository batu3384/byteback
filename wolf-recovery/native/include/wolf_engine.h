#pragma once

#include <string>
#include <cstdint>
#include "wolf_io.h"

namespace wolf {

class Engine {
public:
    Engine();
    ~Engine();

    std::string getVersion() const;
    bool isAdministrator() const;

    DiskReader& getDiskReader() { return diskReader_; }

private:
    std::string version_;
    DiskReader diskReader_;
};

} // namespace wolf
