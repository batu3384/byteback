#pragma once

#include <vector>
#include <mutex>
#include <cstdint>

namespace wolf {

class MemoryPool {
public:
    static MemoryPool& getInstance();

    // Acquire a buffer of at least the requested size
    std::vector<uint8_t>* acquireBuffer(size_t size);

    // Release the buffer back to the pool
    void releaseBuffer(std::vector<uint8_t>* buffer);

private:
    MemoryPool();
    ~MemoryPool();

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    std::mutex m_mutex;
    std::vector<std::vector<uint8_t>*> m_availableBuffers;
    std::vector<std::vector<uint8_t>*> m_allAllocated; // For cleanup
};

} // namespace wolf
