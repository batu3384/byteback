#include "db/runs_codec.h"
#include <cctype>

namespace wolf {

std::string serializeRuns(const std::vector<FileRecord::DataRun>& runs) {
    if (runs.empty()) return "";
    std::string out = "[";
    for (size_t i = 0; i < runs.size(); ++i) {
        if (i) out += ',';
        out += "[" + std::to_string(runs[i].startSector) + "," +
               std::to_string(runs[i].sectorCount) + "]";
    }
    return out + "]";
}

namespace {

bool parseU64(const std::string& s, size_t pos, size_t len, uint64_t& out) {
    if (len == 0 || pos + len > s.size()) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = s[pos + i];
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        uint64_t digit = static_cast<uint64_t>(c - '0');
        if (v > (UINT64_MAX - digit) / 10) return false;
        v = v * 10 + digit;
    }
    out = v;
    return true;
}

} // namespace

std::vector<FileRecord::DataRun> deserializeRuns(const std::string& json) {
    std::vector<FileRecord::DataRun> runs;
    if (json.empty() || json.front() != '[') return runs;

    size_t i = 1;
    while (i < json.size()) {
        while (i < json.size() && (json[i] == ' ' || json[i] == ',')) ++i;
        if (i >= json.size() || json[i] != '[') break;
        ++i;

        size_t c1 = json.find(',', i);
        size_t c2 = json.find(']', i);
        if (c1 == std::string::npos || c2 == std::string::npos || c1 > c2) break;

        FileRecord::DataRun run;
        if (!parseU64(json, i, c1 - i, run.startSector)) break;
        if (!parseU64(json, c1 + 1, c2 - c1 - 1, run.sectorCount)) break;
        if (run.sectorCount == 0) break;

        runs.push_back(run);
        i = c2 + 1;
    }

    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i < json.size() && json[i] != ']') runs.clear();

    return runs;
}

} // namespace wolf
