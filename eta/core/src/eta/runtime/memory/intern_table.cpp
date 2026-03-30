#include "intern_table.h"
#include <eta/runtime/nanbox.h>

namespace eta::runtime::memory::intern {

    std::expected<InternId, InternTableError> InternTable::intern(std::string_view s) {
        const size_t shard_idx = select_shard(s);
        auto&[mtx, str_to_id] = shards[shard_idx];

#if !defined(_MSC_VER)
        // 1. Lock-free fast path for concurrent map
        InternId existing = 0;
        if (str_to_id.visit(s, [&existing](const auto& kv) { existing = kv.second; })) {
            return existing;
        }

        // 2. Prepare data outside the lock
        auto sp = std::make_shared<std::string>(s);
        
        // 3. Generate a new ID. We are okay with wasting an ID on rare collisions
        // to avoid global/shard lock contention.
        InternId id = next_id.fetch_add(1, std::memory_order_relaxed);
        if (id > nanbox::constants::PAYLOAD_MASK) {
            return std::unexpected(InternTableError::IdOutOfRange);
        }

        // 4. Update the ID -> String map first so it's available if needed.
        id_to_str.emplace(id, sp);
        
        // 5. Try to update the String -> ID map.
        InternId result_id = id;
        bool inserted = str_to_id.emplace_or_visit(sp, id, [&](const auto& kv) {
            result_id = kv.second;
        });

        if (!inserted) {
            // Collision: another thread interned the same string.
            // Clean up the orphaned ID to avoid wasting memory.
            id_to_str.erase(id);
            return result_id;
        }
        return id;
#else
        // On MSC_VER, we use a more traditional double-checked locking approach
        // because the map is not concurrent.
        
        // Prepare data outside the lock to minimize critical section time.
        auto sp = std::make_shared<std::string>(s);
        
        std::lock_guard<std::mutex> lock(mtx);

        InternId existing = 0;
        if (str_to_id.visit(s, [&existing](const auto& kv) { existing = kv.second; })) {
            return existing;
        }

        InternId id = next_id.fetch_add(1, std::memory_order_relaxed);
        if (id > nanbox::constants::PAYLOAD_MASK) {
            return std::unexpected(InternTableError::IdOutOfRange);
        }

        id_to_str.emplace(id, sp);
        str_to_id.try_emplace(sp, id);
        return id;
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