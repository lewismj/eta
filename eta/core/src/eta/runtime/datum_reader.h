#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <variant>

#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/runtime/error.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"

namespace eta::runtime {

namespace dr = eta::reader::parser;    ///< alias to avoid pulling in conflicting names

using namespace eta::runtime::nanbox;
using namespace eta::runtime::error;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;

/**
 * @brief Convert a parsed SExpr AST node into a runtime LispVal.
 *
 * Walks the SExpr tree produced by the reader's Parser and builds
 * heap-allocated LispVal objects suitable for the Eta runtime.
 */
inline std::expected<LispVal, RuntimeError>
sexpr_to_value(const dr::SExpr& expr, Heap& heap, InternTable& intern) {
    return std::visit([&](const auto& node) -> std::expected<LispVal, RuntimeError> {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, dr::Nil>) {
            return nanbox::Nil;
        }
        else if constexpr (std::is_same_v<T, dr::Bool>) {
            return node.value ? nanbox::True : nanbox::False;
        }
        else if constexpr (std::is_same_v<T, dr::Char>) {
            auto enc = ops::encode<char32_t>(node.value);
            if (!enc.has_value()) {
                return std::unexpected(VMError{
                    RuntimeErrorCode::InternalError,
                    "datum_reader: invalid character code point"});
            }
            return *enc;
        }
        else if constexpr (std::is_same_v<T, dr::Number>) {
            return std::visit([&](auto n) -> std::expected<LispVal, RuntimeError> {
                using N = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<N, int64_t>) {
                    return make_fixnum(heap, n);
                } else {
                    /// double
                    auto enc = make_flonum(n);
                    if (!enc.has_value()) {
                        return std::unexpected(VMError{
                            RuntimeErrorCode::InternalError,
                            "datum_reader: failed to encode flonum"});
                    }
                    return *enc;
                }
            }, node.value);
        }
        else if constexpr (std::is_same_v<T, dr::String>) {
            auto res = make_string(heap, intern, node.value);
            if (!res.has_value()) return std::unexpected(res.error());
            return *res;
        }
        else if constexpr (std::is_same_v<T, dr::Symbol>) {
            auto res = make_symbol(intern, node.name);
            if (!res.has_value()) return std::unexpected(res.error());
            return *res;
        }
        else if constexpr (std::is_same_v<T, dr::List>) {
            /**
             * Build the list from back to front.
             * Start with the tail (Nil for proper lists, converted tail for dotted).
             */
            LispVal tail = nanbox::Nil;
            if (node.dotted && node.tail) {
                auto tail_res = sexpr_to_value(*node.tail, heap, intern);
                if (!tail_res.has_value()) return std::unexpected(tail_res.error());
                tail = *tail_res;
            }

            /// Walk elements in reverse, consing each onto the tail.
            for (auto it = node.elems.rbegin(); it != node.elems.rend(); ++it) {
                auto elem_res = sexpr_to_value(**it, heap, intern);
                if (!elem_res.has_value()) return std::unexpected(elem_res.error());
                auto cons_res = make_cons(heap, *elem_res, tail);
                if (!cons_res.has_value()) return std::unexpected(cons_res.error());
                tail = *cons_res;
            }
            return tail;
        }
        else if constexpr (std::is_same_v<T, dr::Vector>) {
            std::vector<LispVal> elems;
            elems.reserve(node.elems.size());
            for (const auto& e : node.elems) {
                auto res = sexpr_to_value(*e, heap, intern);
                if (!res.has_value()) return std::unexpected(res.error());
                elems.push_back(*res);
            }
            return make_vector(heap, std::move(elems));
        }
        else if constexpr (std::is_same_v<T, dr::ByteVector>) {
            return make_bytevector(heap, std::vector<uint8_t>(node.bytes));
        }
        else if constexpr (std::is_same_v<T, dr::ReaderForm>) {
            /// Convert reader abbreviations to their list form:
            const char* keyword = nullptr;
            switch (node.kind) {
                case dr::QuoteKind::Quote:           keyword = "quote"; break;
                case dr::QuoteKind::Quasiquote:      keyword = "quasiquote"; break;
                case dr::QuoteKind::Unquote:         keyword = "unquote"; break;
                case dr::QuoteKind::UnquoteSplicing: keyword = "unquote-splicing"; break;
                default: keyword = "quote"; break;
            }

            auto sym_res = make_symbol(intern, keyword);
            if (!sym_res.has_value()) return std::unexpected(sym_res.error());

            auto inner_res = sexpr_to_value(*node.expr, heap, intern);
            if (!inner_res.has_value()) return std::unexpected(inner_res.error());

            /// Build (keyword inner) as two-element list
            auto inner_cons = make_cons(heap, *inner_res, nanbox::Nil);
            if (!inner_cons.has_value()) return std::unexpected(inner_cons.error());
            return make_cons(heap, *sym_res, *inner_cons);
        }
        else if constexpr (std::is_same_v<T, dr::ModuleForm>) {
            /// Module forms are not expected in wire-format data.
            return std::unexpected(VMError{
                RuntimeErrorCode::InternalError,
                "datum_reader: cannot convert module form to runtime value"});
        }
        else {
            return std::unexpected(VMError{
                RuntimeErrorCode::InternalError,
                "datum_reader: unsupported SExpr variant"});
        }
    }, expr.value);
}

/**
 * @brief Parse an s-expression string and convert it to a LispVal.
 *
 * Convenience function that creates a Lexer + Parser, parses one datum,
 * and converts it via sexpr_to_value().
 *
 * @param source The s-expression text to parse.
 * @param heap   The runtime heap for allocating objects.
 * @param intern The intern table for strings and symbols.
 * @return The resulting LispVal, or an error.
 */
inline std::expected<LispVal, RuntimeError>
parse_datum_string(std::string_view source, Heap& heap, InternTable& intern) {
    reader::lexer::Lexer lexer(0, source);
    reader::parser::Parser parser(lexer);

    auto datum_res = parser.parse_datum();
    if (!datum_res.has_value()) {
        /// Map ReaderError to RuntimeError (VMError)
        std::string msg = "datum_reader: parse error: ";
        std::visit([&](const auto& err) {
            using E = std::decay_t<decltype(err)>;
            if constexpr (std::is_same_v<E, reader::parser::ParseError>) {
                msg += reader::parser::to_string(err.kind);
            } else {
                /// LexError
                msg += reader::lexer::to_string(err.kind);
                if (!err.message.empty()) {
                    msg += " â€” ";
                    msg += err.message;
                }
            }
        }, datum_res.error());

        return std::unexpected(VMError{RuntimeErrorCode::InternalError, std::move(msg)});
    }

    if (!*datum_res) {
        return std::unexpected(VMError{
            RuntimeErrorCode::InternalError,
            "datum_reader: parse returned null datum"});
    }

    return sexpr_to_value(**datum_res, heap, intern);
}

} ///< namespace eta::runtime

