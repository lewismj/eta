#include "source.h"

#include <algorithm>
#include <stdexcept>

namespace eta::cst {
    namespace {
        constexpr std::size_t kMapStride = 128;

        struct Scalar {
            std::size_t bytes;
            std::size_t utf16;
            bool valid;
        };

        Scalar scalar(std::string_view text, std::size_t offset, const std::function<void()> &checkpoint = {}) {
            const auto first = static_cast<unsigned char>(text[offset]);
            if (first < 0x80) return {1, 1, true};
            const std::size_t length = first >= 0xF0 && first <= 0xF4
                                           ? 4
                                           : first >= 0xE0 && first <= 0xEF
                                                 ? 3
                                                 : first >= 0xC2 && first <= 0xDF
                                                       ? 2
                                                       : 0;
            std::uint32_t value = length ? first & ((1u << (7 - length)) - 1) : 0;
            bool valid = length && offset + length <= text.size();
            for (std::size_t i = 1; valid && i < length; ++i) {
                const auto byte = static_cast<unsigned char>(text[offset + i]);
                valid = (byte & 0xC0) == 0x80;
                value = (value << 6) | (byte & 0x3F);
            }
            valid = valid && value >= (length == 2 ? 0x80u : length == 3 ? 0x800u : 0x10000u)
                    && value <= 0x10FFFF && !(value >= 0xD800 && value <= 0xDFFF);
            if (valid) return {length, value > 0xFFFF ? 2u : 1u, true};
            std::size_t end = offset + 1;
            while (end < text.size()) {
                if (checkpoint && (end & 4095) == 0) checkpoint();
                const auto byte = static_cast<unsigned char>(text[end]);
                if (byte < 0x80 || (byte >= 0xC2 && byte <= 0xF4)) break;
                ++end;
            }
            return {end - offset, 1, false};
        }
    }

    Source::Source(reader::lexer::FileId id, std::string identity, std::string text,
                   std::optional<std::int64_t> version, std::function<void()> checkpoint)
        : id_(id), identity_(std::move(identity)), text_(std::move(text)), version_(version) {
        std::size_t line_count = 1;
        for (std::size_t i = 0; i < text_.size(); i += 4096) {
            if (checkpoint) checkpoint();
            const auto end = std::min(i + 4096, text_.size());
            line_count += std::count(text_.begin() + i, text_.begin() + end, '\n');
        }
        lines_.reserve(line_count);
        points_.reserve(text_.size() / kMapStride + 2);
        lines_.push_back(0);
        Counts prefix;
        std::size_t next_point = 0;
        std::size_t next_check = 0;
        for (std::size_t i = 0; i < text_.size();) {
            if (i >= next_check) {
                if (checkpoint) checkpoint();
                next_check = i + 4096;
            }
            if (i >= next_point) {
                points_.push_back({i, prefix});
                next_point = i + kMapStride;
            }
            const auto value = scalar(text_, i, checkpoint);
            valid_utf8_ = valid_utf8_ && value.valid;
            if (text_[i] == '\n') lines_.push_back(i + 1);
            ++prefix.scalar;
            prefix.utf16 += value.utf16;
            i += value.bytes;
        }
        points_.push_back({text_.size(), prefix});
    }

    std::string_view Source::slice(ByteSpan span) const {
        if (span.begin > span.end || span.end > text_.size()) throw std::out_of_range("source span");
        return std::string_view(text_).substr(span.begin, span.end - span.begin);
    }

    std::size_t Source::map_memory_bound(std::size_t bytes, std::size_t lines) noexcept {
        return lines * sizeof(std::size_t) + (bytes / kMapStride + 2) * sizeof(Point);
    }

    std::size_t Source::memory_bytes() const noexcept {
        return sizeof(Source) + text_.capacity() + 1 + identity_.capacity() + 1
               + lines_.capacity() * sizeof(std::size_t) + points_.capacity() * sizeof(Point);
    }

    bool Source::valid_utf8() const noexcept { return valid_utf8_; }

    std::expected<Source::Counts, std::string> Source::counts(std::size_t offset) const {
        if (offset > text_.size()) return std::unexpected("offset outside source");
        const auto point = std::upper_bound(points_.begin(), points_.end(), offset,
                                            [](std::size_t value, const Point &item) {
                                                return value < item.offset;
                                            }) - 1;
        auto prefix = point->counts;
        for (auto i = point->offset; i < offset;) {
            const auto value = scalar(text_, i);
            i += value.bytes;
            if (i > offset) return std::unexpected("offset splits a UTF-8 scalar or invalid byte run");
            ++prefix.scalar;
            prefix.utf16 += value.utf16;
        }
        return prefix;
    }

    bool Source::is_edit_boundary(std::size_t offset) const noexcept {
        if (offset > text_.size()) return false;
        if (offset && offset < text_.size() && text_[offset - 1] == '\r' && text_[offset] == '\n') return false;
        const auto point = std::upper_bound(points_.begin(), points_.end(), offset,
                                            [](std::size_t value, const Point &item) {
                                                return value < item.offset;
                                            }) - 1;
        for (auto i = point->offset; i < offset;) {
            i += scalar(text_, i).bytes;
            if (i > offset) return false;
        }
        return true;
    }

    std::expected<SourcePosition, std::string> Source::position(std::size_t offset, PositionEncoding encoding) const {
        if (offset > text_.size()) return std::unexpected("offset outside source");
        const auto line = static_cast<std::size_t>(
            std::upper_bound(lines_.begin(), lines_.end(), offset) - lines_.begin() - 1);
        if (offset && offset < text_.size() && text_[offset - 1] == '\r' && text_[offset] == '\n') --offset;
        const auto prefix = counts(offset);
        if (!prefix) return std::unexpected(prefix.error());
        if (encoding == PositionEncoding::Utf8) return SourcePosition{line, offset - lines_[line]};
        const auto beginning = *counts(lines_[line]);
        return SourcePosition{
            line,
            encoding == PositionEncoding::Utf16 ? prefix->utf16 - beginning.utf16 : prefix->scalar - beginning.scalar
        };
    }

    std::expected<std::size_t, std::string> Source::offset(SourcePosition position, PositionEncoding encoding) const {
        if (position.line >= lines_.size()) return std::unexpected("line outside source");
        const auto begin = lines_[position.line];
        auto end = position.line + 1 < lines_.size() ? lines_[position.line + 1] - 1 : text_.size();
        if (end > begin && end < text_.size() && text_[end - 1] == '\r') --end;
        if (encoding == PositionEncoding::Utf8) {
            if (position.character > end - begin) return std::unexpected("character outside line");
            const auto offset = begin + position.character;
            if (!counts(offset)) return std::unexpected("position splits a Unicode scalar or invalid byte run");
            return offset;
        }
        const auto beginning = *counts(begin);
        const auto ending = *counts(end);
        const bool utf16 = encoding == PositionEncoding::Utf16;
        const auto initial = utf16 ? beginning.utf16 : beginning.scalar;
        const auto final = utf16 ? ending.utf16 : ending.scalar;
        if (position.character > final - initial) return std::unexpected("character outside line");
        const auto target = initial + position.character;
        const auto point = std::upper_bound(points_.begin(), points_.end(), target,
                                            [utf16](std::size_t value, const Point &item) {
                                                return value < (utf16 ? item.counts.utf16 : item.counts.scalar);
                                            }) - 1;
        auto character = utf16 ? point->counts.utf16 : point->counts.scalar;
        auto i = point->offset;
        while (character < target) {
            const auto value = scalar(text_, i);
            character += utf16 ? value.utf16 : 1;
            if (character > target) return std::unexpected("position splits a Unicode scalar");
            i += value.bytes;
        }
        return i;
    }
}
