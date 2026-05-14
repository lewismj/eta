#include <expected>
#include <functional>
#include <sstream>
#include <string>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/primitives/core_primitives_strings_helpers.h"
#include "eta/runtime/csv_builtins.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/json_builtins.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/regex_builtins.h"
#include "eta/runtime/string_view.h"

namespace eta::runtime {

void PrimReg::register_strings() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto& heap = this->heap;
    auto& intern_table = this->intern_table;
    auto* vm = this->vm;

    /**
     * Symbol / string interop.
     */

    env.register_builtin("symbol->string", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto name = get_symbol_name(args[0], intern_table);
        if (!name) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "symbol->string: not a symbol"}});
        }
        return make_string(heap, intern_table, std::string(*name));
    });

    env.register_builtin("string->symbol", 1, false, [&intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto sv = StringView::try_from(args[0], intern_table);
        if (!sv) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "string->symbol: not a string"}});
        }
        return make_symbol(intern_table, std::string(sv->view()));
    });

    /**
     * Higher-order sequence bridges stay between symbol/string interop and the
     * remaining string operations to preserve registration slot order.
     */
    register_sequences_higher_order_bridge();

    /**
     * String operations.
     */

    env.register_builtin("string-length", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto sv = StringView::try_from(args[0], intern_table);
        if (!sv) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "string-length: not a string"}});
        }
        return make_fixnum(heap, static_cast<int64_t>(sv->view().size()));
    });

    env.register_builtin("string-append", 0, true, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        std::string result;
        for (auto value : args) {
            auto sv = StringView::try_from(value, intern_table);
            if (!sv) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "string-append: not a string"}});
            }
            result += sv->view();
        }
        return make_string(heap, intern_table, result);
    });

    env.register_builtin("number->string", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto numeric = classify_numeric(args[0], heap);
        if (!numeric.is_valid()) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "number->string: not a number"}});
        }
        std::string out;
        if (numeric.is_flonum()) {
            std::ostringstream oss;
            oss << numeric.float_val;
            out = oss.str();
        } else {
            out = std::to_string(numeric.int_val);
        }
        return make_string(heap, intern_table, out);
    });

    env.register_builtin("string->number", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto sv = StringView::try_from(args[0], intern_table);
        if (!sv) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "string->number: not a string"}});
        }

        std::string text(sv->view());
        try {
            std::size_t pos = 0;
            const auto integer_value = std::stoll(text, &pos);
            if (pos == text.size()) return make_fixnum(heap, integer_value);
        } catch (...) {
        }
        try {
            std::size_t pos = 0;
            const auto flonum_value = std::stod(text, &pos);
            if (pos == text.size()) return make_flonum(flonum_value);
        } catch (...) {
        }
        return nanbox::False; ///< Scheme convention: return #f on parse failure.
    });

    env.register_builtin("string-ref", 2, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto sv = StringView::try_from(args[0], intern_table);
        if (!sv) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "string-ref: not a string"}});
        }

        auto index = classify_numeric(args[1], heap);
        if (!index.is_valid() || index.is_flonum() || index.int_val < 0) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                "string-ref: index must be a non-negative integer"}});
        }

        const auto view = sv->view();
        if (static_cast<std::size_t>(index.int_val) >= view.size()) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "string-ref: index out of bounds"}});
        }

        const char32_t ch = static_cast<unsigned char>(view[static_cast<std::size_t>(index.int_val)]);
        return ops::encode(ch);
    });

    env.register_builtin("substring", 3, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto sv = StringView::try_from(args[0], intern_table);
        if (!sv) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "substring: not a string"}});
        }

        auto start = classify_numeric(args[1], heap);
        auto end = classify_numeric(args[2], heap);
        if (!start.is_valid() || start.is_flonum() || start.int_val < 0) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                "substring: start must be a non-negative integer"}});
        }
        if (!end.is_valid() || end.is_flonum() || end.int_val < 0) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                "substring: end must be a non-negative integer"}});
        }

        const auto view = sv->view();
        const auto s = static_cast<std::size_t>(start.int_val);
        const auto e = static_cast<std::size_t>(end.int_val);
        if (s > view.size() || e > view.size() || s > e) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "substring: indices out of bounds"}});
        }

        return make_string(heap, intern_table, std::string(view.substr(s, e - s)));
    });

    env.register_builtin("string=?", 2, false, [&intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        return detail::core_primitives_strings::compare_two_strings(
            args, intern_table, "string=?", std::equal_to<>{});
    });

    env.register_builtin("string<?", 2, false, [&intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        return detail::core_primitives_strings::compare_two_strings(
            args, intern_table, "string<?", std::less<>{});
    });

    env.register_builtin("string>?", 2, false, [&intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        return detail::core_primitives_strings::compare_two_strings(
            args, intern_table, "string>?", std::greater<>{});
    });

    env.register_builtin("string<=?", 2, false, [&intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        return detail::core_primitives_strings::compare_two_strings(
            args, intern_table, "string<=?", std::less_equal<>{});
    });

    env.register_builtin("string>=?", 2, false, [&intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        return detail::core_primitives_strings::compare_two_strings(
            args, intern_table, "string>=?", std::greater_equal<>{});
    });

    /**
     * Delegated package-level builtin registrations.
     */
    register_csv_builtins(env, heap, intern_table);
    register_regex_builtins(env, heap, intern_table, vm);
    register_json_builtins(env, heap, intern_table);

    env.register_builtin("char->integer", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::Char) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "char->integer: not a character"}});
        }
        auto ch = ops::decode<char32_t>(args[0]);
        if (!ch) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "char->integer: invalid character"}});
        }
        return make_fixnum(heap, static_cast<int64_t>(*ch));
    });

    env.register_builtin("integer->char", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto numeric = classify_numeric(args[0], heap);
        if (!numeric.is_valid() || numeric.is_flonum() || numeric.int_val < 0) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                "integer->char: not a non-negative integer"}});
        }
        auto encoded = ops::encode(static_cast<char32_t>(numeric.int_val));
        if (!encoded) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "integer->char: encoding failed"}});
        }
        return *encoded;
    });
}

} // namespace eta::runtime
