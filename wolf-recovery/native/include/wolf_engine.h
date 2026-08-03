#pragma once

#include <string>
#include <cstdint>
#include "wolf_io.h"
#include "wolf_db.h"

namespace wolf {

class Engine {
public:
    Engine();
    ~Engine();

    std::string getVersion() const;
    bool isAdministrator() const;

    DiskReader& getDiskReader() { return diskReader_; }
    MetadataStore& getMetadataStore() { return metadataStore_; }

private:
    std::string version_;
    DiskReader diskReader_;
    MetadataStore metadataStore_;
};

} // namespace wolf
