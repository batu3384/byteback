#include "wolf_memory.h"

namespace wolf {

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

std::vector<uint8_t>* MemoryPool::acquireBuffer(size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Find a buffer that is large enough
    for (auto it = m_availableBuffers.begin(); it != m_availableBuffers.end(); ++it) {
        if ((*it)->capacity() >= size) {
            auto buf = *it;
            m_availableBuffers.erase(it);
            buf->resize(size);
            return buf;
        }
    }

    // No suitable buffer found, allocate a new one
    auto newBuf = new std::vector<uint8_t>(size);
    m_allAllocated.push_back(newBuf);
    return newBuf;
}

void MemoryPool::releaseBuffer(std::vector<uint8_t>* buffer) {
    if (!buffer) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_availableBuffers.push_back(buffer);
}

} // namespace wolf
