#include "forensic/nsrl_lookup.h"

#include <cctype>
#include <fstream>

namespace forensic {

bool NsrlLookup::normalizeMd5(std::string& hex) {
    if (hex.size() != 32) return false;
    for (char& c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return true;
}

bool NsrlLookup::loadFromFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;

    hashes_.clear();
    lastPath_ = path;

    auto strip = [](std::string token) {
        while (!token.empty() && (token.front() == '"' || token.front() == ' ' || token.front() == '\t')) {
            token.erase(token.begin());
        }
        while (!token.empty() && (token.back() == '"' || token.back() == ' ' ||
                                  token.back() == '\t' || token.back() == '\r')) {
            token.pop_back();
        }
        return token;
    };

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t start = 0;
        while (start <= line.size()) {
            const auto comma = line.find(',', start);
            const std::string token = strip(
                comma == std::string::npos ? line.substr(start) : line.substr(start, comma - start));
            std::string hex = token;
            if (normalizeMd5(hex)) hashes_.insert(hex);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    return true;
}

void NsrlLookup::clear() {
    hashes_.clear();
    lastPath_.clear();
}

bool NsrlLookup::contains(const std::string& md5Hex) const {
    std::string key = md5Hex;
    if (!normalizeMd5(key)) return false;
    return hashes_.find(key) != hashes_.end();
}

} // namespace forensic
