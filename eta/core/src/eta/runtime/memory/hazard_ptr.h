#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <vector>
#include <algorithm>
#include <ranges>
#include <unordered_map>


//! Minimal implementation for Hazard Ptrs, should be deprecated once
//! Clang/MSVC/gcc support C++26 Hazard Pointers.

namespace eta::runtime::memory::hazard {

    template<typename T>
    class HazardPointerDomain {
        static constexpr int K = 3;         //! Hazard pointers per thread.
        static constexpr int R = K * 128;   //! Retirement batch threshold.

        struct HPRecord {
            std::atomic<std::thread::id> thread_id;
            std::atomic<T*> hazard[K];
            std::atomic<HPRecord*> next;

            HPRecord() : thread_id(std::this_thread::get_id()), next(nullptr) {
                for (int i = 0; i < K; ++i) {
                    hazard[i].store(nullptr, std::memory_order_relaxed);
                }
            }
        };

        struct RetireNode {
            T* ptr;
            std::move_only_function<void(T*)> deleter;
        };

        std::atomic<HPRecord*> head {nullptr};
        inline static thread_local std::unordered_map<const HazardPointerDomain*, HPRecord*> local_records;
        inline static thread_local std::unordered_map<const HazardPointerDomain*, std::vector<RetireNode>> retire_lists;

        HPRecord* acquire_record() noexcept {
            auto it = local_records.find(this);
            if (it != local_records.end()) {
                return it->second;
            }

            HPRecord* record = new HPRecord();
            record->thread_id.store(std::this_thread::get_id(), std::memory_order_relaxed);
            HPRecord* h = head.load(std::memory_order_acquire);
            do {
                record->next.store(h, std::memory_order_relaxed);
            } while (!head.compare_exchange_strong(
                h,
                record,
                std::memory_order_release,
                std::memory_order_acquire));
            local_records[this] = record;
            return record;
        }

        void scan_and_reclaim() {
            std::vector<T*> hazard_pointers;
            HPRecord* record = head.load(std::memory_order_acquire);

            while (record) {
                for (int i = 0; i < K; ++i) {
                    if (T* ptr = record->hazard[i].load(std::memory_order_acquire)) {
                        hazard_pointers.push_back(ptr);
                    }
                }
                record = record->next.load(std::memory_order_acquire);
            }

            std::ranges::sort(hazard_pointers);
            hazard_pointers.erase(
                std::unique(hazard_pointers.begin(), hazard_pointers.end()),
                hazard_pointers.end());

            auto& retire_list = retire_lists[this];

            /// Separate nodes into two lists: safe to delete and still protected
            std::vector<RetireNode> still_protected;
            std::vector<RetireNode> to_delete;

            for (auto& node : retire_list) {
                if (std::ranges::binary_search(hazard_pointers, node.ptr)) {
                    still_protected.push_back(std::move(node));
                } else {
                    to_delete.push_back(std::move(node));
                }
            }

            /// Delete the safe ones
            for (auto& node : to_delete) {
                node.deleter(node.ptr);
            }

            /// Keep only the protected ones
            retire_list = std::move(still_protected);
        }

    public:
        ~HazardPointerDomain() {
            /// Final reclaim for this domain before cleanup
            auto it = retire_lists.find(this);
            if (it != retire_lists.end()) {
                scan_and_reclaim();
                retire_lists.erase(it);
            }
            local_records.erase(this);
        }

        class Handle {
        public:
            Handle(HazardPointerDomain* domain, HPRecord* record, int slot) noexcept
                : domain_(domain), record_(record), slot_(slot) {}

            Handle(const Handle&) = delete;
            Handle& operator=(const Handle&) = delete;

            Handle(Handle&& other) noexcept
                : domain_(other.domain_)
                , record_(std::exchange(other.record_, nullptr))
                , slot_(other.slot_) {}

            Handle& operator=(Handle&& other) noexcept {
                if (this != &other) {
                    release();
                    domain_ = other.domain_;
                    record_ = std::exchange(other.record_, nullptr);
                    slot_ = other.slot_;
                }
                return *this;
            }

            ~Handle() {
                release();
            }

            void acquire(T* ptr) noexcept {
                if (record_) {
                    record_->hazard[slot_].store(ptr, std::memory_order_release);
                    /// Ensure the hazard pointer write is visible before any subsequent operations
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                }
            }

            void release() noexcept {
                if (record_) {
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    record_->hazard[slot_].store(nullptr, std::memory_order_release);
                }
            }

            Handle get_handle(int index = 0) {
                return Handle(domain_, domain_->acquire_record(), index);
            }

            void retire(T* ptr, std::move_only_function<void(T*)> deleter = [](T* p) { delete p; }) {
                if (!ptr) return; ///< Guard against null pointer retirement
                domain_->acquire_record();
                auto& retire_list = retire_lists[domain_];
                retire_list.emplace_back(ptr, std::move(deleter));
                if (retire_list.size() >= R) {
                    domain_->scan_and_reclaim();
                }
            }

            void retire(T* ptr, void (*deleter)(T*)) {
                retire(ptr, std::move_only_function<void(T*)>(deleter));
            }

            void flush() const {
                domain_->acquire_record();
                domain_->scan_and_reclaim();
            }

        private:
            HazardPointerDomain* domain_;
            HPRecord* record_;
            int slot_;
        };

        /// Public API to create a handle
        Handle make_handle(int slot = 0) {
            return Handle(this, acquire_record(), slot);
        }
    };
}
