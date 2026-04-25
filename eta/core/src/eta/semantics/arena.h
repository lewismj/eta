#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <vector>
#include <span>
#include <functional>

namespace eta::semantics::core {

/**
 * @brief Arena allocator for IR nodes
 *
 * Provides stable memory addresses for IR nodes without using raw pointers
 * or manual memory management. All allocations are owned by the arena and
 * freed when the arena is destroyed.
 *
 * Benefits over std::deque<Node>:
 * - Explicit ownership model
 * - Easier to replace nodes during optimization passes
 * - Support for node cloning/transformation
 * - Memory is contiguous within blocks (better cache performance)
 *
 * Note: This arena properly handles non-trivial destructors by registering
 * them at allocation time. When the arena is destroyed or reset, all
 * destructors are called in reverse allocation order.
 */
class Arena {
public:
    static constexpr std::size_t DEFAULT_BLOCK_SIZE = 16 * 1024; ///< 16KB

    explicit Arena(std::size_t block_size = DEFAULT_BLOCK_SIZE)
        : block_size_(block_size) {
        allocate_block();
    }

    ~Arena() {
        call_destructors();
    }

    /// Non-copyable, movable
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    Arena(Arena&& other) noexcept
        : block_size_(other.block_size_)
        , blocks_(std::move(other.blocks_))
        , current_block_(other.current_block_)
        , current_pos_(other.current_pos_)
        , destructors_(std::move(other.destructors_))
    {
        other.current_block_ = nullptr;
        other.current_pos_ = 0;
    }

    Arena& operator=(Arena&& other) noexcept {
        if (this != &other) {
            call_destructors();
            block_size_ = other.block_size_;
            blocks_ = std::move(other.blocks_);
            current_block_ = other.current_block_;
            current_pos_ = other.current_pos_;
            destructors_ = std::move(other.destructors_);
            other.current_block_ = nullptr;
            other.current_pos_ = 0;
        }
        return *this;
    }

    /**
     * @brief Allocate memory for an object of type T and construct it in-place
     * @return Pointer to the constructed object (stable for arena lifetime)
     *
     * For non-trivially destructible types, the destructor is registered and
     * will be called when the arena is destroyed or reset.
     */
    template<typename T, typename... Args>
    T* alloc(Args&&... args) {
        void* ptr = allocate(sizeof(T), alignof(T));
        T* obj = new (ptr) T(std::forward<Args>(args)...);

        /// Register destructor for non-trivial types
        if constexpr (!std::is_trivially_destructible_v<T>) {
            destructors_.push_back([obj]() { obj->~T(); });
        }

        return obj;
    }

    /**
     * @brief Allocate raw bytes with given alignment
     */
    void* allocate(std::size_t size, std::size_t alignment) {
        /// Align current position
        std::size_t aligned_pos = (current_pos_ + alignment - 1) & ~(alignment - 1);

        if (aligned_pos + size > block_size_) {
            /// Need new block
            if (size > block_size_) {
                /// Oversized allocation: create dedicated block
                blocks_.push_back(std::make_unique<std::byte[]>(size));
                return blocks_.back().get();
            }
            allocate_block();
            aligned_pos = 0;
        }

        void* result = current_block_ + aligned_pos;
        current_pos_ = aligned_pos + size;
        return result;
    }

    /**
     * @brief Reset the arena (invalidates all pointers!)
     *
     * Calls all registered destructors in reverse order before clearing memory.
     */
    void reset() {
        call_destructors();
        blocks_.clear();
        allocate_block();
    }

    /**
     * @brief Get total allocated bytes
     */
    [[nodiscard]] std::size_t total_bytes() const noexcept {
        return blocks_.size() * block_size_;
    }

private:
    void allocate_block() {
        blocks_.push_back(std::make_unique<std::byte[]>(block_size_));
        current_block_ = blocks_.back().get();
        current_pos_ = 0;
    }

    void call_destructors() {
        /// Call destructors in reverse order (LIFO)
        for (auto it = destructors_.rbegin(); it != destructors_.rend(); ++it) {
            (*it)();
        }
        destructors_.clear();
    }

    std::size_t block_size_;
    std::vector<std::unique_ptr<std::byte[]>> blocks_;
    std::byte* current_block_{nullptr};
    std::size_t current_pos_{0};
    std::vector<std::function<void()>> destructors_;
};

/**
 * @brief Smart pointer for arena-allocated IR nodes
 *
 * This is a non-owning pointer that can be used to reference nodes
 * in the arena. The arena owns all nodes and frees them when destroyed.
 *
 * Using this wrapper instead of raw Node* provides:
 * - Type safety
 * - Explicit semantics (NodeRef vs Node*)
 * - Potential for additional validation in debug builds
 */
template<typename T>
class Ref {
public:
    Ref() noexcept = default;
    explicit Ref(T* ptr) noexcept : ptr_(ptr) {}

    T* get() const noexcept { return ptr_; }
    T& operator*() const noexcept { return *ptr_; }
    T* operator->() const noexcept { return ptr_; }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    bool operator==(const Ref&) const noexcept = default;

    /// Allow implicit conversion to T* for compatibility
    operator T*() const noexcept { return ptr_; }

private:
    T* ptr_{nullptr};
};

/**
 * @brief Deduction guide for Ref
 */
template<typename T>
Ref(T*) -> Ref<T>;

} ///< namespace eta::semantics::core

