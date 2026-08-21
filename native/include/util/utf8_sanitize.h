#pragma once

#include <string>

namespace byteback {

// N-API String::New aborts/throws on illegal UTF-8 (NTFS names). Replace
// broken bytes with '?' so JS callbacks cannot kill the process.
inline std::string utf8ForJs(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        int need = 0;
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else {
            out.push_back('?');
            ++i;
            continue;
        }
        if (i + static_cast<size_t>(need) >= n) {
            out.push_back('?');
            break;
        }
        bool ok = true;
        for (int k = 1; k <= need; ++k) {
            if ((p[i + static_cast<size_t>(k)] & 0xC0) != 0x80) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            out.push_back('?');
            ++i;
            continue;
        }
        out.append(s, i, static_cast<size_t>(need) + 1);
        i += static_cast<size_t>(need) + 1;
    }
    return out;
}

} // namespace byteback
