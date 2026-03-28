#include "intern_table.h"
#include <eta/runtime/nanbox.h>

namespace eta::runtime::memory::intern {

    std::expected<InternId, InternTableError> InternTable::intern(std::string_view s) noexcept {
        const size_t shard_idx = select_shard(s);
        auto&[mtx, str_to_id] = shards[shard_idx];

        InternId existing = 0;
        {
#if defined(_MSC_VER)
            std::lock_guard<std::mutex> lock(mtx);
#endif
            if (str_to_id.visit(s, [&existing](const auto& kv) { existing = kv.second;})) {
                return existing;
            }
        }

        // Slow path: need to insert
        auto sp = std::make_shared<std::string>(s);

        // Allocate ID on the fast path: fetch first, then check.
        InternId id = next_id.fetch_add(1, std::memory_order_relaxed);
        if (id > nanbox::constants::PAYLOAD_MASK) {
            return std::unexpected(InternTableError::IdOutOfRange);
        }

        // Critical section: atomically insert into both maps
        {
            std::lock_guard<std::mutex> lock(mtx);

            // Double-check: another thread might have inserted while we waited.
            InternId race_check = 0;
            if (str_to_id.visit(sp, [&race_check](const auto& kv) {
                race_check = kv.second;
            })) {
                // Lost race - return winner's id.
                return race_check;
            }

            // We won - insert into both maps under lock. Order matters: id_to_str first, then str_to_id.
            // This ensures readers never see dangling references.
            id_to_str.emplace(id, sp);
            str_to_id.try_emplace(sp, id);

            return id;
        }
    }

    std::expected<InternId, InternTableError> InternTable::get_id(std::string_view s) const noexcept {
        const size_t shard_idx = select_shard(s);
        const auto&[mtx, str_to_id] = shards[shard_idx];

        InternId result = 0;
        {
#if defined(_MSC_VER)
            std::lock_guard<std::mutex> lock(mtx);
#endif
            if (str_to_id.visit(s, [&result](const auto& kv) {
                result = kv.second;
            })) {
                return result;
            }
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