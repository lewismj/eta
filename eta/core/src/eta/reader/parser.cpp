#include "parser.h"
#include <charconv>
#include "sexpr_utils.h"


using enum eta::reader::lexer::Token::Kind;
using namespace eta::reader::lexer;

namespace eta::reader::parser {

    using namespace utils;

    Parser::Parser(lexer::Lexer& lexer, bool strict_quasiquote)
        : lexer_(lexer), qq_strict_(strict_quasiquote) {}

    std::expected<Token, ReaderError> Parser::advance() {
        if (lookahead_) {
            Token tok = std::move(*lookahead_);
            lookahead_.reset();
            return tok;
        }
        return  lexer_.next_token();
    }

    std::expected<Token, ReaderError> Parser::peek() {
        if (lookahead_) return *lookahead_;
        auto res = lexer_.next_token();
        if (!res) {
            return res;
        }
        lookahead_ = *res;
        return *lookahead_;
    }

    Span Parser::merge_spans(Span a, Span b) {
        Span out = a; out.end = b.end; return out;
    }

    static SExprPtr box(SExprValue v) {
        auto n = std::make_unique<SExpr>();
        n->value = std::move(v);
        return n;
    }

    // (no local cloning utilities needed)

    std::expected<std::vector<SExprPtr>, ReaderError > Parser::parse_toplevel() {
        std::vector<SExprPtr> out;
        while (true) {
            auto t = peek();
            if (!t) return std::unexpected(t.error());
            if (t->kind == EOF_) break;

            auto datum = parse_datum();
            if (!datum) return std::unexpected(datum.error());

            // Recognize (module ...)
            if ((*datum) && (*datum)->is<List>()) {
                auto* lst = (*datum)->as<List>();
                if (lst) {
                    if (auto mod = try_parse_module(*lst)) {
                        ModuleForm mf = std::move(*mod);
                        mf.span = lst->span; // cover whole list
                        out.push_back(box(SExprValue{std::move(mf)}));
                        continue;
                    }
                }
            }
            out.push_back(std::move(*datum));
        }
        return out;
    }

    std::expected<SExprPtr, ReaderError> Parser::parse_datum() {
        auto t = peek();
        if (!t) return std::unexpected(t.error());

        switch (t->kind) {
            case LParen: {
                auto open = advance();
                if (!open) return std::unexpected(open.error());
                return parse_list(RParen, open->span);
            }
            case LBracket: {
                auto open = advance();
                if (!open) return std::unexpected(open.error());
                return parse_list(RBracket, open->span);
            }
            case VectorStart: {
                auto open = advance();
                if (!open) return std::unexpected(open.error());
                return parse_vector(open->span);
            }
            case ByteVectorStart: {
                auto open = advance();
                if (!open) return std::unexpected(open.error());
                return parse_byte_vector(open->span);
            }
            case Quote:
            case Backtick:
            case Comma:
            case CommaAt: {
                auto ab = advance();
                if (!ab) return std::unexpected(ab.error());
                return parse_abbreviation(*ab);
            }
            case Boolean:
            case Token::Kind::Char:
            case Token::Kind::String:
            case Token::Kind::Symbol:
            case Token::Kind::Number: {
                auto at = advance();
                if (!at) return std::unexpected(at.error());
                return parse_atom(*at);
            }
            case RParen:
            case RBracket:
                return std::unexpected(ParseError {ParseErrorKind::UnexpectedClosingDelimiter, t->span});
            case Dot:
                return std::unexpected(ParseError {ParseErrorKind::MisplacedDot, t->span});
            case EOF_:
                return std::unexpected(ParseError {ParseErrorKind::UnexpectedEOF, t->span});
            default:
                return std::unexpected(ParseError {ParseErrorKind::UnsupportedToken, t->span});
        }
    }

    std::expected<SExprPtr, ReaderError> Parser::parse_list(Token::Kind close_kind, Span open_span) {
        List lst; lst.span = open_span; // temp; finalize with closer

        while (true) {
            auto t = peek();
            if (!t) return std::unexpected(t.error());
            if (t->kind == EOF_) {
                return std::unexpected(ParseError{ParseErrorKind::UnclosedList, open_span});
            }
            if (t->kind == close_kind) {
                auto close = advance();
                if (!close) return std::unexpected(close.error());
                lst.span = merge_spans(open_span, close->span);
                return box(SExprValue{std::move(lst)});
            }
            // If we see a closing delimiter that doesn't match the expected one,
            // treat it as an unclosed list for the current opener, as tests expect.
            if (t->kind == RParen || t->kind == RBracket) {
                return std::unexpected(ParseError{ParseErrorKind::UnclosedList, open_span});
            }
            if (t->kind == Dot) {
                if (lst.dotted) {
                    return std::unexpected(ParseError{ParseErrorKind::MultipleDotsInDottedList, t->span});
                }
                if (lst.elems.empty()) {
                    return std::unexpected(ParseError{ParseErrorKind::DotAtListStart, t->span});
                }
                (void)advance(); // consume '.'
                auto tail = parse_datum();
                if (!tail) return std::unexpected(tail.error());
                lst.dotted = true;
                lst.tail = std::move(*tail);
                continue; // expect the closer next; loop handles it
            }
            auto item = parse_datum();
            if (!item) return std::unexpected(item.error());

            if (lst.dotted) {
                return std::unexpected(ParseError{ParseErrorKind::MisplacedDot, (*item)->span()});
            }

            lst.elems.push_back(std::move(*item));
        }
    }

    std::expected<SExprPtr, ReaderError> Parser::parse_vector(Span open_span) {
        Vector vec; vec.span = open_span;
        while (true) {
            auto t = peek();
            if (!t) return std::unexpected(t.error());
            if (t->kind == EOF_) return std::unexpected(ParseError{ParseErrorKind::UnclosedVector, open_span});
            if (t->kind == RParen) {
                auto close = advance();
                if (!close) return std::unexpected(close.error());
                vec.span = merge_spans(open_span, close->span);
                return box(SExprValue{std::move(vec)});
            }
            if (t->kind == Dot) return std::unexpected(ParseError{ParseErrorKind::DotInVector, t->span});
            auto item = parse_datum();
            if (!item) return std::unexpected(item.error());
            vec.elems.push_back(std::move(*item));
        }
    }

    static std::uint8_t parse_byte_literal(const NumericToken& n, const Span& s, std::string& err) {
        using K = NumericToken::Kind;
        if (n.kind != K::Fixnum) { err = "byte must be an integer"; return 0; }
        unsigned long value = 0;
        const char* first = n.text.c_str();
        const char* last  = first + n.text.size();
        const int base = n.radix;
        if (base == 10 || base == 16) {
            std::from_chars_result r{};
            if (base == 10) r = std::from_chars(first, last, value, 10);
            else            r = std::from_chars(first, last, value, 16);
            if (r.ec != std::errc{}) { err = "invalid integer"; return 0; }
        } else {
            value = 0;
            for (const char* p = first; p != last; ++p) {
                unsigned d = 0; char c = *p;
                if (c >= '0' && c <= '9') d = unsigned(c - '0');
                else if (c >= 'a' && c <= 'f') d = 10u + unsigned(c - 'a');
                else if (c >= 'A' && c <= 'F') d = 10u + unsigned(c - 'A');
                else { err = "invalid digit"; return 0; }
                if (d >= unsigned(base)) { err = "digit out of range"; return 0; }
                value = value * unsigned(base) + d;
            }
        }
        if (value > 255ul) { err = "byte out of range"; return 0; }
        (void)s;
        return static_cast<std::uint8_t>(value);
    }

    std::expected<SExprPtr, ReaderError> Parser::parse_byte_vector(Span open_span) {
        ByteVector bv; bv.span = open_span;
        while (true) {
            auto t = peek();
            if (!t) return std::unexpected(t.error());
            if (t->kind == EOF_) return std::unexpected(ParseError{ParseErrorKind::UnclosedVector, open_span});
            if (t->kind == RParen) {
                auto close = advance();
                if (!close) return std::unexpected(close.error());
                bv.span = merge_spans(open_span, close->span);
                return box(SExprValue{std::move(bv)});
            }
            if (t->kind != Token::Kind::Number) {
                return std::unexpected(ParseError{ParseErrorKind::ByteVectorNonInteger, t->span});
            }
            auto tok = advance();
            if (!tok) return std::unexpected(tok.error());
            const auto& num = std::get<NumericToken>(tok->value);
            std::string why;
            auto byte = parse_byte_literal(num, tok->span, why);
            if (!why.empty()) return std::unexpected(ParseError{ParseErrorKind::InvalidByteLiteral, tok->span});
            bv.bytes.push_back(byte);
        }
    }

    std::expected<SExprPtr, ReaderError> Parser::parse_atom(const Token& tok) {
        using enum Token::Kind;
        switch (tok.kind) {
            case Boolean: {
                Bool b; b.span = tok.span; b.value = std::get<bool>(tok.value);
                return box(SExprValue{std::move(b)});
            }
            case Char: {
                parser::Char c; c.span = tok.span; c.value = std::get<char32_t>(tok.value);
                return box(SExprValue{std::move(c)});
            }
            case String: {
                parser::String s; s.span = tok.span; s.value = std::get<std::string>(tok.value);
                return box(SExprValue{std::move(s)});
            }
            case Symbol: {
                std::string sym = std::get<std::string>(tok.value);
                if (sym == "Nil") { Nil n; n.span = tok.span; return box(SExprValue{std::move(n)}); }
                parser::Symbol s; s.span = tok.span; s.name = std::move(sym);
                return box(SExprValue{std::move(s)});
            }
            case Number: {
                const auto& num = std::get<NumericToken>(tok.value);
                eta::Number val;
                if (num.kind == NumericToken::Kind::Fixnum) {
                    try {
                        val = static_cast<int64_t>(std::stoll(num.text, nullptr, num.radix));
                    } catch (...) {
                        // If it fails to parse as stoll (e.g. out of range), fallback to double or error
                        // For now, let's just use stod as fallback if it's too large for int64
                        val = std::stod(num.text);
                    }
                } else {
                    val = std::stod(num.text);
                }
                parser::Number n; n.span = tok.span; n.value = val;
                return box(SExprValue{std::move(n)});
            }
            // Not atoms - these are structural tokens handled by parse_datum
            case LParen:
            case RParen:
            case LBracket:
            case RBracket:
            case Quote:
            case Backtick:
            case Comma:
            case CommaAt:
            case Dot:
            case VectorStart:
            case ByteVectorStart:
            case EOF_:
                return std::unexpected(ParseError{ParseErrorKind::InternalNotAnAtom, tok.span});
        }
        // Unreachable but satisfies compilers that don't understand exhaustive switches
        return std::unexpected(ParseError{ParseErrorKind::InternalNotAnAtom, tok.span});
    }

    std::expected<SExprPtr, ReaderError> Parser::parse_abbreviation(const Token& tok) {
        auto parse_in_ctx = [&](const QuoteKind k) -> std::expected<SExprPtr, ReaderError> {
            if (qq_strict_ && (k == QuoteKind::Unquote || k == QuoteKind::UnquoteSplicing) && qq_depth_ == 0) {
                return std::unexpected(ParseError{ParseErrorKind::UnquoteOutsideQuasiquote, tok.span});
            }
            if (k == QuoteKind::Quasiquote) ++qq_depth_;
            auto expr = parse_datum();
            if (!expr) return expr;
            if (k == QuoteKind::Quasiquote) --qq_depth_;

            ReaderForm rf; rf.kind = k; rf.span = merge_spans(tok.span, (*expr)->span()); rf.expr = std::move(*expr);
            return box(SExprValue{std::move(rf)});
        };

        using enum Token::Kind;
        switch (tok.kind) {
            case Quote:    return parse_in_ctx(QuoteKind::Quote);
            case Backtick: return parse_in_ctx(QuoteKind::Quasiquote);
            case Comma:    return parse_in_ctx(QuoteKind::Unquote);
            case CommaAt:  return parse_in_ctx(QuoteKind::UnquoteSplicing);
            // Not abbreviation tokens - these should never reach this function
            case LParen:
            case RParen:
            case LBracket:
            case RBracket:
            case Boolean:
            case Char:
            case String:
            case Symbol:
            case Number:
            case Dot:
            case VectorStart:
            case ByteVectorStart:
            case EOF_:
                return std::unexpected(ParseError{ParseErrorKind::InternalNotAReaderToken, tok.span});
        }
        // Unreachable but satisfies compilers that don't understand exhaustive switches
        return std::unexpected(ParseError{ParseErrorKind::InternalNotAReaderToken, tok.span});
    }


    std::optional<ModuleForm> Parser::try_parse_module(List& list) {
        if (list.dotted) return std::nullopt;
        if (list.elems.size() < 3) return std::nullopt;

        const auto* head = as_symbol(list.elems[0]);
        if (!head || head->name != "module") return std::nullopt;

        const auto* name_sym = as_symbol(list.elems[1]);
        if (!name_sym) return std::nullopt;

        ModuleForm mod; mod.span = list.span; mod.name = name_sym->name;

        for (std::size_t i = 2; i < list.elems.size(); ++i) {
            auto* sub = as_list(list.elems[i]);
            if (!sub || sub->elems.empty()) continue;
            const auto* tag = as_symbol(sub->elems[0]);
            if (!tag) continue;

            if (tag->name == "export") {
                for (std::size_t j = 1; j < sub->elems.size(); ++j) {
                    if (auto* s = as_symbol(sub->elems[j])) mod.exports.push_back(s->name);
                }
            } else if (tag->name == "begin") {
                for (std::size_t j = 1; j < sub->elems.size(); ++j) {
                    mod.body.push_back(std::move(sub->elems[j]));
                }
            } else {
                // Preserve other top-level forms (e.g., define, import)
                mod.body.push_back(std::move(list.elems[i]));
            }
        }

        return mod;
    }

} // namespace eta::reader