#include "eta/runtime/prof/frame_id.h"

#include <functional>
#include <mutex>

namespace eta::runtime::prof {

namespace {

inline void hash_combine(std::size_t& seed, std::size_t value) noexcept {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

} ///< namespace

std::size_t FrameIdInterner::FrameKeyHash::operator()(const FrameKey& key) const noexcept {
    std::size_t seed = 0;
    hash_combine(seed, std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(key.kind)));
    hash_combine(seed, std::hash<std::string>{}(key.qualified_name));
    hash_combine(seed, std::hash<std::uint32_t>{}(key.source_span.file_id));
    hash_combine(seed, std::hash<std::uint32_t>{}(key.source_span.start.line));
    hash_combine(seed, std::hash<std::uint32_t>{}(key.source_span.start.column));
    hash_combine(seed, std::hash<std::uint32_t>{}(key.source_span.end.line));
    hash_combine(seed, std::hash<std::uint32_t>{}(key.source_span.end.column));
    return seed;
}

FrameId FrameIdInterner::intern(const FrameKey& key) {
    {
        std::shared_lock read_lock(mutex_);
        if (const auto it = key_to_id_.find(key); it != key_to_id_.end()) {
            return it->second;
        }
    }

    std::unique_lock write_lock(mutex_);
    if (const auto it = key_to_id_.find(key); it != key_to_id_.end()) {
        return it->second;
    }

    const auto id = static_cast<FrameId>(id_to_key_.size());
    id_to_key_.push_back(key);
    key_to_id_.emplace(id_to_key_.back(), id);
    return id;
}

std::optional<FrameId> FrameIdInterner::lookup(const FrameKey& key) const {
    std::shared_lock lock(mutex_);
    if (const auto it = key_to_id_.find(key); it != key_to_id_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<FrameKey> FrameIdInterner::key_for(const FrameId id) const {
    std::shared_lock lock(mutex_);
    if (id >= id_to_key_.size()) return std::nullopt;
    return id_to_key_[id];
}

std::size_t FrameIdInterner::size() const {
    std::shared_lock lock(mutex_);
    return id_to_key_.size();
}

void FrameIdInterner::clear() {
    std::unique_lock lock(mutex_);
    key_to_id_.clear();
    id_to_key_.clear();
}

const char* to_string(const FrameKind kind) noexcept {
    switch (kind) {
        case FrameKind::EtaFunction: return "eta-function";
        case FrameKind::Builtin: return "builtin";
        case FrameKind::AnonymousLambda: return "anonymous-lambda";
        case FrameKind::TopLevel: return "top-level";
        case FrameKind::ContinuationResume: return "continuation-resume";
        case FrameKind::UserRegion: return "user-region";
    }
    return "unknown";
}

} ///< namespace eta::runtime::prof
