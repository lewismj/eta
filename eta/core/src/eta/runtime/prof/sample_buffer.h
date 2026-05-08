#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace eta::runtime::prof {

/**
 * @brief One sampled stack snapshot from a VM thread.
 */
struct SampleRecord {
    std::uint64_t timestamp_ns{0};
    std::vector<std::uint32_t> stack_frame_ids;
};

/**
 * @brief Bounded mailbox for sampled stack snapshots.
 *
 * This phase keeps the API stable while using a simple mutex-protected queue.
 */
class SampleBuffer {
public:
    explicit SampleBuffer(std::size_t capacity = 4096);

    /**
     * @brief Push a sample into the buffer.
     * @return false if the buffer is full.
     */
    [[nodiscard]] bool push(SampleRecord sample);

    /**
     * @brief Pop one sample from the buffer.
     * @return false when the buffer is empty.
     */
    [[nodiscard]] bool try_pop(SampleRecord& out);

    /**
     * @brief Remove all buffered samples.
     */
    void clear();

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<SampleRecord> queue_;
};

} ///< namespace eta::runtime::prof

