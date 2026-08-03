#pragma once

#include <string>
#include <cstdint>

namespace wolf {

class Engine {
public:
    Engine();
    ~Engine();

    std::string getVersion() const;
    bool isAdministrator() const;

private:
    std::string version_;
};

} // namespace wolf
