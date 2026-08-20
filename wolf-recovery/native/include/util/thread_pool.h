#pragma once

#include <algorithm>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

namespace wolf {
namespace util {

// ponytail: cap at 4 workers — carve is I/O bound; more threads rarely help on HDD.
inline unsigned defaultCarveWorkers() {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    return std::min(4u, std::max(1u, hw));
}

template <typename Fn>
void parallelFor(unsigned workers, Fn&& fn) {
    if (workers <= 1) {
        fn(0, 1);
        return;
    }
    std::vector<std::future<void>> pending;
    pending.reserve(workers);
    for (unsigned w = 0; w < workers; ++w) {
        pending.push_back(std::async(std::launch::async, [&, w]() { fn(w, workers); }));
    }
    for (auto& f : pending) f.get();
}

} // namespace util
} // namespace wolf
