#include "intern_table.h"
#include <eta/runtime/nanbox.h>

namespace eta::runtime::memory::intern {

    std::expected<InternId, InternTableError> InternTable::intern(std::string_view s) {
        const size_t shard_idx = select_shard(s);
        auto&[mtx, str_to_id] = shards[shard_idx];

        // Common logic to check if already interned (lock-free read on non-MSVC,
        // must be called under lock on MSVC)
        auto try_find_existing = [&]() -> std::optional<InternId> {
            InternId existing = 0;
            if (str_to_id.visit(s, [&existing](const auto& kv) { existing = kv.second; })) {
                return existing;
            }
            return std::nullopt;
        };

        // Common logic to create new entry (must be called under lock)
        auto create_new_entry = [&](const auto& sp) -> std::expected<InternId, InternTableError> {
            InternId id = next_id.fetch_add(1, std::memory_order_relaxed);
            if (id > nanbox::constants::PAYLOAD_MASK) {
                return std::unexpected(InternTableError::IdOutOfRange);
            }
            // Insert into both maps under lock. Order matters: id_to_str first, then str_to_id.
            // This ensures readers never see dangling references.
            id_to_str.emplace(id, sp);
            str_to_id.try_emplace(sp, id);
            return id;
        };

#if defined(_MSC_VER)
        // MSVC path: std::unordered_map is not thread-safe, must always take lock
        std::lock_guard<std::mutex> lock(mtx);

        if (auto existing = try_find_existing()) {
            return *existing;
        }

        auto sp = std::make_shared<std::string>(s);
        return create_new_entry(sp);
#else
        // Non-MSVC path: concurrent_flat_map is thread-safe for reads
        // Fast path: check if already interned (lock-free)
        if (auto existing = try_find_existing()) {
            return *existing;
        }

        // Slow path: need to insert - take the lock
        auto sp = std::make_shared<std::string>(s);

        // Critical section: allocate ID and insert atomically
        {
            std::lock_guard<std::mutex> lock(mtx);

            // Double-check: another thread might have inserted while we waited.
            InternId race_check = 0;
            if (str_to_id.visit(sp, [&race_check](const auto& kv) {
                race_check = kv.second;
            })) {
                // Lost race - return winner's id (no ID wasted).
                return race_check;
            }

            return create_new_entry(sp);
        }
#endif
    }

    std::expected<InternId, InternTableError> InternTable::get_id(std::string_view s) const noexcept {
        const size_t shard_idx = select_shard(s);
        const auto&[mtx, str_to_id] = shards[shard_idx];

        // Concurrent map is thread-safe for reads, no lock needed
        InternId result = 0;
        if (str_to_id.visit(s, [&result](const auto& kv) {
            result = kv.second;
        })) {
            return result;
        }
        return std::unexpected(InternTableError::MissingId);
    }


    std::expected<std::string_view, InternTableError> InternTable::get_string(const InternId& id) const noexcept {
        std::string_view result;
        if (id_to_str.visit(id, [&result](const auto& kv) {
            result = *kv.second;
        })) {
            return result;
        }
        return std::unexpected(InternTableError::MissingString);
    }


}