#pragma once

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "eta/reader/lexer.h"

namespace eta::runtime::prof {

/**
 * @brief Profiler frame category.
 */
enum class FrameKind : std::uint8_t {
    EtaFunction,
    Builtin,
    AnonymousLambda,
    TopLevel,
    ContinuationResume,
    UserRegion,
};

/**
 * @brief Stable frame descriptor used for interned profiler frame IDs.
 */
struct FrameKey {
    FrameKind kind{FrameKind::EtaFunction};
    std::string qualified_name;
    eta::reader::lexer::Span source_span{};

    [[nodiscard]] bool operator==(const FrameKey& other) const noexcept {
        return kind == other.kind &&
               qualified_name == other.qualified_name &&
               source_span.file_id == other.source_span.file_id &&
               source_span.start.line == other.source_span.start.line &&
               source_span.start.column == other.source_span.start.column &&
               source_span.end.line == other.source_span.end.line &&
               source_span.end.column == other.source_span.end.column;
    }
};

/**
 * @brief Dense frame ID type used by profiler tables.
 */
using FrameId = std::uint32_t;

/**
 * @brief Thread-safe interner for profiler frame descriptors.
 */
class FrameIdInterner {
public:
    /**
     * @brief Intern @p key and return its stable dense ID.
     */
    [[nodiscard]] FrameId intern(const FrameKey& key);

    /**
     * @brief Lookup an existing ID for @p key.
     */
    [[nodiscard]] std::optional<FrameId> lookup(const FrameKey& key) const;

    /**
     * @brief Retrieve an interned descriptor by ID.
     */
    [[nodiscard]] std::optional<FrameKey> key_for(FrameId id) const;

    /**
     * @brief Number of interned frame IDs.
     */
    [[nodiscard]] std::size_t size() const;

    /**
     * @brief Remove all interned frame IDs.
     */
    void clear();

private:
    struct FrameKeyHash {
        [[nodiscard]] std::size_t operator()(const FrameKey& key) const noexcept;
    };

    mutable std::shared_mutex mutex_;
    std::unordered_map<FrameKey, FrameId, FrameKeyHash> key_to_id_;
    std::vector<FrameKey> id_to_key_;
};

/**
 * @brief Render @p kind as text for diagnostics and JSON output.
 */
[[nodiscard]] const char* to_string(FrameKind kind) noexcept;

} ///< namespace eta::runtime::prof
