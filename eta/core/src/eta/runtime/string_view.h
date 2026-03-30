#pragma once

#include <expected>
#include <string_view>
#include <variant>

#include <eta/runtime/nanbox.h>
#include <eta/runtime/error.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/types/types.h>

namespace eta::runtime {

using namespace eta::runtime::nanbox;
using namespace eta::runtime::error;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;

/**
 * @brief Unified string abstraction for the runtime
 *
 * This class provides a type-safe wrapper around both interned and heap-allocated
 * strings, ensuring consistent handling across the VM, Emitter, and other runtime
 * components.
 *
 * Usage:
 *   auto sv = StringView::from(val, intern_table, heap);
 *   if (sv) {
 *       std::string_view str = sv->view();
 *       // use str...
 *   }
 */
class StringView {
public:
    /**
     * @brief Create a StringView from a LispVal (non-throwing)
     * @return StringView if the value is a string, std::nullopt otherwise
     */
    static std::optional<StringView> try_from(
        LispVal v,
        InternTable& intern_table,
        Heap& heap
    ) noexcept {
        if (!ops::is_boxed(v)) return std::nullopt;

        Tag t = ops::tag(v);
        if (t == Tag::String) {
            auto str_opt = intern_table.get_string(ops::payload(v));
            if (str_opt) return StringView{*str_opt, StringKind::Interned};
            return std::nullopt;
        }

        if (t == Tag::HeapObject) {
            HeapEntry entry;
            if (heap.try_get(ops::payload(v), entry) &&
                entry.header.kind == ObjectKind::String) {
                auto* str = static_cast<types::String*>(entry.ptr);
                return StringView{std::string_view(str->value), StringKind::Heap};
            }
        }
        return std::nullopt;
    }

    /**
     * @brief Create a StringView from a LispVal
     * @return StringView if the value is a string, error otherwise
     */
    static std::expected<StringView, RuntimeError> from(
        LispVal v,
        InternTable& intern_table,
        Heap& heap
    ) {
        auto res = try_from(v, intern_table, heap);
        if (res) return *res;
        return std::unexpected(VMError{RuntimeErrorCode::TypeError, "Not a string"});
    }

    /**
     * @brief Check if a LispVal is a string (interned or heap)
     */
    static bool is_string(LispVal v, Heap& heap) noexcept {
        if (!ops::is_boxed(v)) return false;
        Tag t = ops::tag(v);
        if (t == Tag::String) return true;
        if (t == Tag::HeapObject) {
            HeapEntry entry;
            return heap.try_get(ops::payload(v), entry) && entry.header.kind == ObjectKind::String;
        }
        return false;
    }

    /**
     * @brief Compare two strings for equality
     */
    static std::expected<bool, RuntimeError> equal(
        LispVal a, LispVal b,
        InternTable& intern_table,
        Heap& heap
    ) {
        // Fast path: if both are the same value, they're equal
        if (a == b) return true;

        auto sv_a = from(a, intern_table, heap);
        if (!sv_a) return std::unexpected(sv_a.error());

        auto sv_b = from(b, intern_table, heap);
        if (!sv_b) return std::unexpected(sv_b.error());

        return sv_a->view() == sv_b->view();
    }

    // Accessors
    [[nodiscard]] std::string_view view() const noexcept { return view_; }
    [[nodiscard]] bool is_interned() const noexcept { return kind_ == StringKind::Interned; }
    [[nodiscard]] bool is_heap() const noexcept { return kind_ == StringKind::Heap; }

    // Implicit conversion to string_view
    operator std::string_view() const noexcept { return view_; }

    // String operations (delegated to string_view)
    [[nodiscard]] std::size_t size() const noexcept { return view_.size(); }
    [[nodiscard]] bool empty() const noexcept { return view_.empty(); }
    [[nodiscard]] const char* data() const noexcept { return view_.data(); }

private:
    enum class StringKind : std::uint8_t { Interned, Heap };

    StringView(std::string_view view, StringKind kind)
        : view_(view), kind_(kind) {}

    std::string_view view_;
    StringKind kind_;
};

/**
 * @brief Check if a LispVal is a symbol
 */
inline bool is_symbol(LispVal v) noexcept {
    return ops::is_boxed(v) && ops::tag(v) == Tag::Symbol;
}

/**
 * @brief Get symbol name from a LispVal
 */
inline std::expected<std::string_view, RuntimeError> get_symbol_name(
    LispVal v,
    InternTable& intern_table
) {
    if (!is_symbol(v)) {
        return std::unexpected(VMError{RuntimeErrorCode::TypeError, "Not a symbol"});
    }

    auto str_opt = intern_table.get_string(ops::payload(v));
    if (str_opt) {
        return *str_opt;
    }
    return std::unexpected(VMError{RuntimeErrorCode::TypeError, "Invalid symbol ID"});
}

} // namespace eta::runtime

