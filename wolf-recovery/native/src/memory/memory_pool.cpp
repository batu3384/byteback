#include "wolf_memory.h"

namespace wolf {

void BufferDeleter::operator()(std::vector<uint8_t>* ptr) const {
    if (ptr) {
        MemoryPool::getInstance().releaseBuffer(ptr);
    }
}

MemoryPool& MemoryPool::getInstance() {
    static MemoryPool instance;
    return instance;
}

MemoryPool::MemoryPool() {}

MemoryPool::~MemoryPool() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto buf : m_allAllocated) {
        delete buf;
    }
    m_allAllocated.clear();
    m_availableBuffers.clear();
}

PoolBufferPtr MemoryPool::acquireBuffer(size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Find a buffer that is large enough using multimap (O(log N))
    auto it = m_availableBuffers.lower_bound(size);
    if (it != m_availableBuffers.end()) {
        auto buf = it->second;
        m_availableBuffers.erase(it);
        buf->resize(size);
        return PoolBufferPtr(buf, BufferDeleter());
    }

    // No suitable buffer found, allocate a new one
    auto newBuf = new std::vector<uint8_t>(size);
    m_allAllocated.push_back(newBuf);
    return PoolBufferPtr(newBuf, BufferDeleter());
}

void MemoryPool::releaseBuffer(std::vector<uint8_t>* buffer) {
    if (!buffer) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_availableBuffers.insert({buffer->capacity(), buffer});
}

} // namespace wolf
