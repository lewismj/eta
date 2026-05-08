#include "eta/runtime/prof/sample_buffer.h"

namespace eta::runtime::prof {

SampleBuffer::SampleBuffer(const std::size_t capacity)
    : capacity_(capacity) {}

bool SampleBuffer::push(SampleRecord sample) {
    std::lock_guard lock(mutex_);
    if (queue_.size() >= capacity_) return false;
    queue_.push_back(std::move(sample));
    return true;
}

bool SampleBuffer::try_pop(SampleRecord& out) {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void SampleBuffer::clear() {
    std::lock_guard lock(mutex_);
    queue_.clear();
}

std::size_t SampleBuffer::size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

} ///< namespace eta::runtime::prof

