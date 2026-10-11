#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "eta/reader/lexer.h"

namespace eta::cst {
    struct ByteSpan {
        std::size_t begin{};
        std::size_t end{};

        bool operator==(const ByteSpan &) const = default;
    };

    enum class PositionEncoding : std::uint8_t { UnicodeScalar, Utf16, Utf8 };

    /// Zero-based line and character. CLI adds one to UnicodeScalar positions.
    struct SourcePosition {
        std::size_t line{};
        std::size_t character{};

        bool operator==(const SourcePosition &) const = default;
    };

    class Source {
    public:
        Source(reader::lexer::FileId id, std::string identity, std::string text,
               std::optional<std::int64_t> version = {}, std::function<void()> checkpoint = {});

        [[nodiscard]] reader::lexer::FileId id() const noexcept { return id_; }
        [[nodiscard]] const std::string &identity() const noexcept { return identity_; }
        [[nodiscard]] std::optional<std::int64_t> version() const noexcept { return version_; }
        [[nodiscard]] std::string_view text() const noexcept { return text_; }

        [[nodiscard]] std::string_view slice(ByteSpan span) const;

        [[nodiscard]] std::size_t memory_bytes() const noexcept;
        [[nodiscard]] static std::size_t map_memory_bound(std::size_t bytes, std::size_t lines) noexcept;

        [[nodiscard]] bool valid_utf8() const noexcept;

        [[nodiscard]] bool is_edit_boundary(std::size_t offset) const noexcept;

        [[nodiscard]] std::expected<SourcePosition, std::string> position(
            std::size_t offset, PositionEncoding encoding) const;

        [[nodiscard]] std::expected<std::size_t, std::string> offset(
            SourcePosition position, PositionEncoding encoding) const;

    private:
        reader::lexer::FileId id_;
        std::string identity_;
        std::string text_;
        std::optional<std::int64_t> version_;
        std::vector<std::size_t> lines_;
        struct Counts { std::size_t scalar{}; std::size_t utf16{}; };
        struct Point { std::size_t offset{}; Counts counts{}; };
        std::vector<Point> points_;
        bool valid_utf8_{true};
        [[nodiscard]] std::expected<Counts, std::string> counts(std::size_t offset) const;
    };
}
