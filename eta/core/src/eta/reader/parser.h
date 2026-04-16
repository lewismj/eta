#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <ostream>
#include <variant>
#include <vector>

#include "types.h"
#include "eta/reader/lexer.h"

namespace eta::reader::parser {

    using lexer::Span;
    using lexer::Token;
    using lexer::NumericToken;
    using lexer::LexError;

    // Forward declare the SExpr node and pointer alias
    struct SExpr;
    using SExprPtr = std::unique_ptr<SExpr>;

    enum class ParseErrorKind : std::uint8_t {
        // Generic/propagation
        FromLexer,

        // Token/stream conditions
        UnexpectedEOF,
        UnexpectedClosingDelimiter,
        UnsupportedToken,

        // List-specific
        UnclosedList,
        MisplacedDot,
        MultipleDotsInDottedList,
        DotAtListStart,

        // Vector/#u8()-specific
        UnclosedVector,
        DotInVector,
        ByteVectorNonInteger,
        InvalidByteLiteral,

        // Numeric parsing
        InvalidNumericLiteral,  ///< stoll/stod failed on a numeric token

        // Reader forms / quoting
        UnquoteOutsideQuasiquote,

        // Internal consistency errors
        InternalNotAnAtom,
        InternalNotAReaderToken
    };

    struct ParseError {
        ParseErrorKind kind{};
        Span span{};

        ParseError(const ParseErrorKind k, const Span &s)
            : kind(k), span(s) {}
    };

    constexpr const char* to_string(ParseErrorKind kind) noexcept {
        using enum ParseErrorKind;
        switch (kind) {
            case FromLexer:                  return "ParseErrorKind::FromLexer";
            case UnexpectedEOF:              return "ParseErrorKind::UnexpectedEOF";
            case UnexpectedClosingDelimiter: return "ParseErrorKind::UnexpectedClosingDelimiter";
            case UnsupportedToken:           return "ParseErrorKind::UnsupportedToken";
            case UnclosedList:               return "ParseErrorKind::UnclosedList";
            case MisplacedDot:               return "ParseErrorKind::MisplacedDot";
            case MultipleDotsInDottedList:   return "ParseErrorKind::MultipleDotsInDottedList";
            case DotAtListStart:             return "ParseErrorKind::DotAtListStart";
            case UnclosedVector:             return "ParseErrorKind::UnclosedVector";
            case DotInVector:                return "ParseErrorKind::DotInVector";
            case ByteVectorNonInteger:       return "ParseErrorKind::ByteVectorNonInteger";
            case InvalidByteLiteral:         return "ParseErrorKind::InvalidByteLiteral";
            case InvalidNumericLiteral:      return "ParseErrorKind::InvalidNumericLiteral";
            case UnquoteOutsideQuasiquote:   return "ParseErrorKind::UnquoteOutsideQuasiquote";
            case InternalNotAnAtom:          return "ParseErrorKind::InternalNotAnAtom";
            case InternalNotAReaderToken:    return "ParseErrorKind::InternalNotAReaderToken";
        }
        return "ParseErrorKind::Unknown";
    }

    inline std::ostream& operator<<(std::ostream& os, ParseErrorKind kind) {
        return os << to_string(kind);
    }

    using ReaderError = std::variant<ParseError, LexError>;

    struct NodeBase { Span span{}; };

    struct Nil       : NodeBase {};
    struct Bool      : NodeBase { bool value{}; };
    struct Char      : NodeBase { char32_t value{}; };
    struct String    : NodeBase { std::string value; };
    struct Symbol    : NodeBase { std::string name; };

    struct Number : NodeBase { eta::Number value; };

    // (a b c) or dotted (a b . c)
    struct List : NodeBase {
        std::vector<SExprPtr> elems; // head elements
        bool dotted{false};
        SExprPtr tail;               // valid if dotted==true
    };

    struct Vector : NodeBase {
        std::vector<SExprPtr> elems;
    };

    struct ByteVector : NodeBase {
        std::vector<std::uint8_t> bytes;
    };

    enum class QuoteKind : std::uint8_t { Quote, Quasiquote, Unquote, UnquoteSplicing };

    struct ReaderForm : NodeBase {
        QuoteKind kind{};
        SExprPtr expr;
    };

    // Owns a normalized representation of a recognized module body
    struct ModuleForm : NodeBase {
        std::string name;                 // e.g. "std.collections"
        std::vector<std::string> exports; // names only
        std::vector<SExprPtr> body;       // owning copies of body forms
    };

    // The S-expression variant
    using SExprValue = std::variant<
        Nil,
        Bool, Char, String, Symbol, Number,
        List, Vector, ByteVector,
        ReaderForm, ModuleForm
    >;

    struct SExpr {
        SExprValue value;

        [[nodiscard]] Span span() const noexcept {
            return std::visit([](const auto& n) { return n.span; }, value);
        }
        template <typename T> [[nodiscard]] bool is()  const noexcept { return std::holds_alternative<T>(value); }
        template <typename T> [[nodiscard]] const T* as() const noexcept { return std::get_if<T>(&value); }
        template <typename T> [[nodiscard]] T*       as()       noexcept { return std::get_if<T>(&value); }
    };

    /**
     * @brief Perform a deep copy of an S-expression to ensure memory safety.
     * Centralized utility used by expander and semantic analyzer.
     */
    SExprPtr deep_copy(const SExpr& expr);

    /**
     * @brief Helper for deep copying from a smart pointer.
     */
    inline SExprPtr deep_copy(const SExprPtr& expr) {
        return expr ? deep_copy(*expr) : nullptr;
    }

    class Parser {
    public:
        explicit Parser(lexer::Lexer& lexer, bool strict_quasiquote = false);

        // Zero or more top-level forms until EOF
        std::expected<std::vector<SExprPtr>, ReaderError > parse_toplevel();

        // One datum (useful for REPL/tests)
        std::expected<SExprPtr, ReaderError> parse_datum();

    private:
        lexer::Lexer& lexer_;
        std::optional<Token> lookahead_;
        int qq_depth_{0};
        bool qq_strict_{false};

        // Token management
        std::expected<Token, ReaderError > peek();
        std::expected<Token, ReaderError > advance();

        // Parsing routines
        std::expected<SExprPtr, ReaderError> parse_list(Token::Kind close_kind, Span open_span);
        std::expected<SExprPtr, ReaderError> parse_vector(Span open_span);
        std::expected<SExprPtr, ReaderError> parse_byte_vector(Span open_span);
        std::expected<SExprPtr, ReaderError> parse_atom(const Token& tok);
        std::expected<SExprPtr, ReaderError> parse_abbreviation(const Token& tok);

        // Post-parse recognition
        // Note: May move elements out of the provided list when a module is recognized.
        // The caller discards the original datum afterward, so this is safe.
        std::optional<ModuleForm> try_parse_module(List& list);

        // Span utility
        static Span merge_spans(Span open, Span close);
    };

} // namespace eta::reader