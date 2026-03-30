#pragma once

#include <array>
#include <atomic>
#include <expected>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>

#include <boost/container_hash/hash.hpp>
#include <boost/unordered/concurrent_flat_map.hpp>

namespace eta::runtime::memory::intern {
    using InternId = std::uint64_t;

    enum class InternTableError : std::uint8_t {
        MissingId,
        MissingString,
        IdOutOfRange,
    };

    constexpr const char* to_string(const InternTableError error) noexcept {
        using enum InternTableError;
        switch (error) {
            case MissingId: return "InternTableError::MissingId";
            case MissingString: return "InternTableError::MissingString";
            case IdOutOfRange: return "InternTableError::IdOutOfRange";
            default: return "InternTableError::Unknown";
        }
    }

    inline std::ostream& operator<<(std::ostream& os, const InternTableError e) {
        return os << to_string(e);
    }

    class InternTable {
    public:
        InternTable() = default;
        ~InternTable() = default;

        //! Non-copyable, Non-movable.
        InternTable(const InternTable&) = delete;
        InternTable& operator=(const InternTable&) = delete;
        InternTable(InternTable&&) = delete;
        InternTable& operator=(InternTable&&) = delete;

        //! Intern a string, returning its InternId.
        //! Note: Not noexcept because allocations (make_shared, map insertion) can throw.
        std::expected<InternId, InternTableError> intern(std::string_view s);

        //! Look up id by string.
        std::expected<InternId, InternTableError> get_id(std::string_view s) const noexcept;

        //! Look up string by id.
        std::expected<std::string_view, InternTableError> get_string(const InternId& id) const noexcept;

    private:
        using SPtr = std::shared_ptr<std::string>;

#if defined(_MSC_VER)
        template <typename K, typename V, typename H, typename E>
        struct MapWrapper {
            std::unordered_map<K, V, H, E> map;
            
            template <typename Key, typename Func>
            bool visit(const Key& k, Func&& f) const {
                // This wrapper itself doesn't have the lock, the Shard has it.
                // But the caller in InternTable needs to know if they need to lock.
                auto it = map.find(k);
                if (it != map.end()) {
                    f(*it);
                    return true;
                }
                return false;
            }

            template <typename Key, typename... Args>
            auto try_emplace(Key&& k, Args&&... args) {
                return map.try_emplace(std::forward<Key>(k), std::forward<Args>(args)...);
            }
        };
        template <typename K, typename V, typename H, typename E>
        using map_t = MapWrapper<K, V, H, E>;
#else
        template <typename K, typename V, typename H, typename E>
        using map_t = boost::unordered::concurrent_flat_map<K, V, H, E>;
#endif

        //! Hash for string->id.
        struct Hash {
#ifdef NDEBUG
            static inline const std::size_t seed = std::random_device{}();
#else
            //! For debug builds we want deterministic hashes.
            static constexpr std::size_t seed = 0xC0C0C0C0;
#endif
            using is_transparent = void;

            size_t operator()(const SPtr& p) const noexcept {
                std::size_t h = seed;
                boost::hash_combine(h, boost::hash_range(p->begin(), p->end()));
                return h;
            }

            size_t operator()(std::string_view sv) const noexcept {
                std::size_t h = seed;
                boost::hash_combine(h, boost::hash_range(sv.begin(), sv.end()));
                return h;
            }
        };

        //! Eq for string->id.
        struct Eq {
            using is_transparent = void;

            bool operator()(const SPtr& a, const SPtr& b) const noexcept {
                return *a == *b;
            }
            bool operator()(const SPtr& a, std::string_view b) const noexcept {
                return std::string_view{*a} == b;
            }
            bool operator()(std::string_view a, const SPtr& b) const noexcept {
                return a == std::string_view{*b};
            }
        };

        //! These should be configurable, but 128 is a reasonable guess.
        static constexpr size_t NUM_SHARDS = 128;
        static constexpr size_t SHARD_MASK = NUM_SHARDS - 1;

        struct Shard {
            mutable std::mutex mtx;
            map_t<SPtr, InternId, Hash, Eq> str_to_id;
        };

        //! Select shard for a given string
        size_t select_shard(std::string_view s) const noexcept {
            return hasher(s) & SHARD_MASK;
        }

        //! Sharded string->id maps with per-shard locks
        std::array<Shard, NUM_SHARDS> shards;

        //! Global id->string map (lock-free reads after insertion)
        boost::unordered::concurrent_flat_map<InternId, SPtr> id_to_str;

        //! Next id available (monotonic)
        std::atomic<InternId> next_id{0};

        //! Hash function for selecting shard
        Hash hasher;
    };

}