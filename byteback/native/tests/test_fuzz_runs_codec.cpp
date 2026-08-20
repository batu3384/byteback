#include "db/runs_codec.h"

#include <gtest/gtest.h>
#include <random>
#include <string>

using namespace byteback;

namespace {
std::string mutate(const std::string& base, std::mt19937& rng) {
    if (base.empty()) return base;
    std::string out = base;
    const int ops = static_cast<int>(rng() % 5) + 1;
    for (int i = 0; i < ops; ++i) {
        const size_t pos = static_cast<size_t>(rng() % (out.size() + 1));
        const char c = static_cast<char>(rng() % 96 + 32);
        out.insert(pos, 1, c);
    }
    return out;
}
} // namespace

TEST(RunsCodecFuzz, RandomMutationsNeverThrow) {
    const std::vector<std::string> seeds = {
        "", "not json", "[[1,2]]", "[[0,1],[4096,16]]",
        "[[999999999999999999999999999999,1]]", "[[1,0]]", "[[abc,2]]",
    };

    std::mt19937 rng(0xC0FFEE);
    for (int i = 0; i < 2000; ++i) {
        const auto& seed = seeds[static_cast<size_t>(rng() % seeds.size())];
        const std::string input = mutate(seed, rng);
        const auto runs = deserializeRuns(input);
        if (!runs.empty()) {
            const auto json = serializeRuns(runs);
            const auto round = deserializeRuns(json);
            ASSERT_EQ(round.size(), runs.size());
        }
    }
}
