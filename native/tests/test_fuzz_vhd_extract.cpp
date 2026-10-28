#include "io/vhd_source.h"

#include <gtest/gtest.h>
#include <random>
#include <string>
#include <vector>

using namespace byteback;

namespace {

std::vector<uint8_t> mutate(std::vector<uint8_t> base, std::mt19937& rng) {
    if (base.empty()) base.push_back(static_cast<uint8_t>(rng() & 0xff));
    const int ops = static_cast<int>(rng() % 6) + 1;
    for (int i = 0; i < ops; ++i) {
        const size_t pos = static_cast<size_t>(rng() % (base.size() + 1));
        base.insert(base.begin() + static_cast<std::ptrdiff_t>(pos),
                    static_cast<uint8_t>(rng() & 0xff));
    }
    return base;
}

} // namespace

TEST(VhdExtractFuzz, RandomMutationsNeverThrow) {
    std::vector<std::vector<uint8_t>> seeds = {
        std::vector<uint8_t>(512, 0),
        wrapFixedVhd({0x55, 0xAA, 0x01, 0x02}),
        wrapVhdx({0x01, 0x02, 0x03}, false),
    };

    std::mt19937 rng(0xBEEF);
    for (int i = 0; i < 1500; ++i) {
        const auto& seed = seeds[static_cast<size_t>(rng() % seeds.size())];
        const auto blob = mutate(seed, rng);
        std::vector<uint8_t> payload;
        std::string err;
        (void)extractVhdPayload(blob.data(), blob.size(), payload, err);
        (void)extractFixedVhdPayload(blob.data(), blob.size(), payload, err);
    }
}
