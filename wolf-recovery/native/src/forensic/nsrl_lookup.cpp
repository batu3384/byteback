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

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        // NSRL RDS CSV rows embed MD5 in the first column; accept bare hex too.
        std::string token = line;
        const auto comma = token.find(',');
        if (comma != std::string::npos) token = token.substr(0, comma);
        if (!normalizeMd5(token)) continue;
        hashes_.insert(token);
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
