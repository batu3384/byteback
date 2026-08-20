#pragma once

#include <vector>
#include <mutex>
#include <cstdint>
#include <memory>
#include <map>

namespace byteback {

class MemoryPool;

struct BufferDeleter {
    void operator()(std::vector<uint8_t>* ptr) const;
};

using PoolBufferPtr = std::unique_ptr<std::vector<uint8_t>, BufferDeleter>;

class MemoryPool {
public:
    static MemoryPool& getInstance();

    // Acquire a buffer of at least the requested size
    PoolBufferPtr acquireBuffer(size_t size);

    // Release the buffer back to the pool
    void releaseBuffer(std::vector<uint8_t>* buffer);

private:
    MemoryPool();
    ~MemoryPool();

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    std::mutex m_mutex;
    std::multimap<size_t, std::vector<uint8_t>*> m_availableBuffers;
    std::vector<std::vector<uint8_t>*> m_allAllocated; // For cleanup
};

} // namespace byteback
