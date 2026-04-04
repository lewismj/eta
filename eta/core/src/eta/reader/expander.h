#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <ostream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "eta/reader/parser.h"
#include "sexpr_utils.h"
#include "error_format.h"

namespace eta::reader::expander {

    using parser::Span;
    using parser::SExpr;
    using parser::SExprPtr;
    using parser::Symbol;
    using parser::List;
    using parser::Vector;
    using parser::ByteVector;
    using parser::ReaderForm;
    using parser::ModuleForm;

    /**
     * @brief Expansion error
     *
     * Note: Consider migrating to eta::diagnostic::Diagnostic for unified
     * error handling across all compiler phases.
     */
    struct ExpandError {
        enum class Kind : std::uint8_t {
            InvalidSyntax,
            DuplicateIdentifier,
            InvalidLetBindings,
            ReservedKeyword,
            ArityError,
            ExpansionDepthExceeded
        };
        Kind kind{};
        Span span{};
        std::string message;
    };

    // Enum printer and full formatter for ExpandError
    constexpr const char* to_string(ExpandError::Kind k) noexcept {
        using enum ExpandError::Kind;
        switch (k) {
            case InvalidSyntax:          return "ExpandError::Kind::InvalidSyntax";
            case DuplicateIdentifier:    return "ExpandError::Kind::DuplicateIdentifier";
            case InvalidLetBindings:     return "ExpandError::Kind::InvalidLetBindings";
            case ReservedKeyword:        return "ExpandError::Kind::ReservedKeyword";
            case ArityError:             return "ExpandError::Kind::ArityError";
            case ExpansionDepthExceeded: return "ExpandError::Kind::ExpansionDepthExceeded";
        }
        return "ExpandError::Kind::Unknown";
    }


    inline std::ostream& operator<<(std::ostream& os, const ExpandError& e) {
        os << to_string(e.kind) << " at ";
        write_span(os, e.span);
        if (!e.message.empty()) os << ": " << e.message;
        return os;
    }

    template <typename T>
    using ExpanderResult = std::expected<T, ExpandError>;

    struct ExpanderConfig {
        //! If true, rewrite leading internal defines in lambda bodies to letrec
        //! per Scheme internal-define semantics.
        bool enable_internal_defines_to_letrec{true};
        // Expansion limits
        std::size_t depth_limit{10000};
    };

    struct Formals {
        std::vector<std::string> fixed;     //! (x y)
        std::optional<std::string> rest;    //! dotted tail or rest-only symbol
        Span span{};
    };

    // ========================================================================
    // syntax-rules data structures
    // ========================================================================

    /// A single element in a syntax-rules pattern
    struct SyntaxPattern;
    using SyntaxPatternPtr = std::unique_ptr<SyntaxPattern>;

    struct PatVar        { std::string name; };                       // pattern variable (binds)
    struct PatUnderscore {};                                          // _ (matches anything, no binding)
    struct PatLiteral    { std::string name; };                       // literal keyword (matches by name)
    struct PatDatum      { SExprPtr datum; };                         // literal constant (number, bool, etc.)
    struct PatList {
        std::vector<SyntaxPatternPtr> elems;                         // fixed elements
        std::optional<SyntaxPatternPtr> ellipsis_pat;                // sub-pattern before ...
        std::size_t ellipsis_index{0};                               // index where ... appears
    };

    struct SyntaxPattern {
        std::variant<PatVar, PatUnderscore, PatLiteral, PatDatum, PatList> data;
    };

    /// A single element in a syntax-rules template
    struct SyntaxTemplate;
    using SyntaxTemplatePtr = std::unique_ptr<SyntaxTemplate>;

    struct TmplVar     { std::string name; };                        // substitute bound pattern variable
    struct TmplDatum   { SExprPtr datum; };                          // literal constant copied verbatim
    struct TmplSymbol  { std::string name; };                        // introduced identifier (subject to hygiene)
    struct TmplList {
        std::vector<SyntaxTemplatePtr> elems;                        // fixed elements
        std::optional<SyntaxTemplatePtr> ellipsis_tmpl;              // sub-template before ...
        std::size_t ellipsis_index{0};                               // index where ... appears
    };

    struct SyntaxTemplate {
        std::variant<TmplVar, TmplDatum, TmplSymbol, TmplList> data;
    };

    /// One clause: (pattern template)
    struct SyntaxClause {
        SyntaxPatternPtr pattern;
        SyntaxTemplatePtr tmpl;
        Span span{};
    };

    /// A complete syntax-rules transformer
    struct SyntaxRulesTransformer {
        std::vector<std::string> literals;                           // literal keywords
        std::vector<SyntaxClause> clauses;
    };

    /// Result of pattern matching: pattern variable -> bound value(s)
    struct MatchBinding {
        SExprPtr single;                                             // for non-ellipsis vars
        std::vector<SExprPtr> repeated;                              // for ellipsis vars
        bool is_ellipsis{false};
    };
    using MatchEnv = std::unordered_map<std::string, MatchBinding>;

    /**
     * @brief Macro expander and desugaring pass for the eta language
     *
     * The Expander is the CANONICAL place for desugaring derived forms.
     * After expansion, the output should only contain Core IR forms that
     * the SemanticAnalyzer expects.
     *
     * ## Desugaring Transformations
     *
     * The following derived forms are desugared by the Expander:
     *
     * ### Binding Forms
     * - `let`      -> `((lambda (x...) body...) init...)`
     * - `let*`     -> nested `let`
     * - `letrec`   -> `(let ((x '()) ...) (set! x e) ... body...)`
     * - `letrec*`  -> nested `letrec`
     * - Named `let` (loop) -> `(letrec ((name (lambda (x...) body...))) (name init...))`
     *
     * ### Control Flow
     * - `cond`     -> nested `if`
     * - `case`     -> `(let ((tmp key)) (if (or (eqv? tmp d1) ...) body ...))`
     * - `and`      -> nested `if` with short-circuit
     * - `or`       -> nested `if` with short-circuit + temp binding
     * - `when`     -> `(if test (begin body...) (begin))`
     * - `unless`   -> `(if test (begin) (begin body...))`
     *
     * ### Iteration
     * - `do`       -> `(letrec ((loop (lambda (x...) (if test result (begin body (loop step...)))))) (loop init...))`
     *
     * ### Other
     * - `quasiquote` -> combination of `quote`, `cons`, `append`, `list`
     * - Internal defines -> `letrec` at lambda body start
     * - `define-syntax` with `syntax-rules` -> user-defined hygienic macros
     *
     * ## Output (Core IR)
     *
     * After expansion, the output contains only these primitive forms:
     * - `if`, `begin`, `set!`, `lambda`, `quote`, `apply`
     * - `dynamic-wind`, `values`, `call-with-values`, `call/cc`
     * - Function application
     * - Module directives: `module`, `export`, `import`, `define`
     */
    class Expander {
    public:
        explicit Expander(ExpanderConfig cfg = {});

        ExpanderResult<SExprPtr> expand_form(const SExprPtr& in);
        ExpanderResult<std::vector<SExprPtr>> expand_many(const std::vector<SExprPtr>& forms);

    private:
        ExpanderConfig cfg_{};
        std::size_t depth_{0};

        //! Macro environment: maps macro name -> transformer (populated by define-syntax)
        std::unordered_map<std::string, SyntaxRulesTransformer> macro_env_;


        // Error helpers
        static ExpandError syntax_error(Span sp, std::string_view msg, std::string hint = {});
        static ExpandError arity_error(Span sp, std::string_view form, std::size_t expected, std::size_t got);
        static ExpandError invalid_syntax(Span sp, std::string_view form, std::string_view expected);

        // Shared identifier validation helper
        // Checks for reserved keywords and optional duplicate detection
        // Returns error if validation fails, otherwise returns void
        static ExpanderResult<void> validate_identifier(
            const std::string& name, Span span,
            std::unordered_set<std::string>* seen = nullptr,
            std::string_view context = "identifier");

        // Shared expansion utilities
        ExpanderResult<std::vector<SExprPtr>> expand_list_elems(const std::vector<SExprPtr>& elems) const;

        ExpanderResult<SExprPtr> expand_application(const List& lst);

        //! Special forms.
        ExpanderResult<SExprPtr> handle_quote_like(const List& lst); // (quote ...); ReaderForm passed through elsewhere
        ExpanderResult<SExprPtr> handle_if(const List& lst);
        ExpanderResult<SExprPtr> handle_begin(const List& lst);
        ExpanderResult<SExprPtr> handle_define(const List& lst);
        ExpanderResult<SExprPtr> handle_set_bang(const List& lst);
        ExpanderResult<SExprPtr> handle_lambda(const List& lst);
        ExpanderResult<SExprPtr> handle_let(const List& lst);
        ExpanderResult<SExprPtr> handle_let_star(const List& lst);
        ExpanderResult<SExprPtr> handle_letrec(const List& lst);
        ExpanderResult<SExprPtr> handle_letrec_star(const List& lst);
        ExpanderResult<SExprPtr> handle_cond(const List& lst);
        ExpanderResult<SExprPtr> handle_case(const List& lst);
        ExpanderResult<SExprPtr> handle_and(const List& lst);
        ExpanderResult<SExprPtr> handle_or(const List& lst);
        ExpanderResult<SExprPtr> handle_when(const List& lst);
        ExpanderResult<SExprPtr> handle_unless(const List& lst);
        ExpanderResult<SExprPtr> handle_do(const List& lst);
        ExpanderResult<SExprPtr> handle_define_record_type(const List& lst);
        ExpanderResult<SExprPtr> handle_define_syntax(const List& lst);


        //! Modules/directives
        ExpanderResult<SExprPtr> handle_module_list(const List& lst);    // (module name ...)
        ExpanderResult<SExprPtr> handle_export(const List& lst);
        ExpanderResult<SExprPtr> handle_import(const List& lst);

        // Convenience sugars explicitly requested
        ExpanderResult<SExprPtr> handle_def(const List& lst);   // (def ...) sugar → define
        ExpanderResult<SExprPtr> handle_defun(const List& lst); // (defun name (args) body...)

        // -- Helpers --
        static SExprPtr make_symbol(std::string name, Span s);
        static SExprPtr make_nil(Span s);
        static SExprPtr make_list(std::vector<SExprPtr> elems, Span s);
        static SExprPtr make_dotted_list(std::vector<SExprPtr> head, SExprPtr tail, Span s);

        // Variadic helper to build lists more concisely
        template<typename... Args>
        static SExprPtr build_list(Span s, Args&&... args) {
            std::vector<SExprPtr> v;
            v.reserve(sizeof...(args));
            (v.push_back(std::forward<Args>(args)), ...);
            return make_list(std::move(v), s);
        }

        // Convenience: build a form like (keyword arg1 arg2 ...)
        template<typename... Args>
        static SExprPtr make_form(Span s, const char* keyword, Args&&... args) {
            return build_list(s, make_symbol(keyword, s), std::forward<Args>(args)...);
        }

        // Build (if test conseq alt)
        static SExprPtr make_if(Span s, SExprPtr test, SExprPtr conseq, SExprPtr alt) {
            return make_form(s, "if", std::move(test), std::move(conseq), std::move(alt));
        }

        // Build (begin body...)
        static SExprPtr make_begin(Span s, std::vector<SExprPtr> body) {
            std::vector<SExprPtr> v;
            v.reserve(body.size() + 1);
            v.push_back(make_symbol("begin", s));
            for (auto& e : body) v.push_back(std::move(e));
            return make_list(std::move(v), s);
        }

        // Build (let ((name init) ...) body...)
        static SExprPtr make_let(Span s, std::vector<std::pair<SExprPtr, SExprPtr>> bindings, std::vector<SExprPtr> body) {
            auto bindingList = make_list({}, s);
            for (auto& [name, init] : bindings) {
                bindingList->as<List>()->elems.push_back(
                    build_list(name->span(), std::move(name), std::move(init)));
            }
            std::vector<SExprPtr> v;
            v.reserve(body.size() + 2);
            v.push_back(make_symbol("let", s));
            v.push_back(std::move(bindingList));
            for (auto& e : body) v.push_back(std::move(e));
            return make_list(std::move(v), s);
        }

        // Build (lambda formals body...)
        static SExprPtr make_lambda(Span s, SExprPtr formals, std::vector<SExprPtr> body) {
            std::vector<SExprPtr> v;
            v.reserve(body.size() + 2);
            v.push_back(make_symbol("lambda", s));
            v.push_back(std::move(formals));
            for (auto& e : body) v.push_back(std::move(e));
            return make_list(std::move(v), s);
        }

        // Build (set! name value)
        static SExprPtr make_set(Span s, SExprPtr name, SExprPtr value) {
            return make_form(s, "set!", std::move(name), std::move(value));
        }

        // Build (letrec ((name init) ...) body...)
        static SExprPtr make_letrec(Span s, std::vector<std::pair<SExprPtr, SExprPtr>> bindings, std::vector<SExprPtr> body) {
            auto bindingList = make_list({}, s);
            for (auto& [name, init] : bindings) {
                bindingList->as<List>()->elems.push_back(
                    build_list(name->span(), std::move(name), std::move(init)));
            }
            std::vector<SExprPtr> v;
            v.reserve(body.size() + 2);
            v.push_back(make_symbol("letrec", s));
            v.push_back(std::move(bindingList));
            for (auto& e : body) v.push_back(std::move(e));
            return make_list(std::move(v), s);
        }

        static SExprPtr deep_clone(const SExpr& n) { return parser::deep_copy(n); }
        static SExprPtr deep_clone(const SExprPtr& p) { return parser::deep_copy(p); }

        static bool is_reserved(std::string_view name);
        static std::string gensym(const std::string& hint = "t");

        ExpanderResult<Formals> parse_formals(const SExprPtr& node) const;

        ExpanderResult<std::vector<std::pair<SExprPtr, SExprPtr>>>
        parse_let_pairs(const List& pair_list, bool require_unique_names) const;

        //! Named-let detection: (let name ((x e) ...) body...)
        static bool is_named_let_syntax(const List& lst);

        //! Internal defines -> letrec in lambda bodies
        ExpanderResult<bool> rewrite_internal_defines_to_letrec(std::vector<SExprPtr>& body) const;

        // Quasiquote expansion helpers
        ExpanderResult<SExprPtr> expand_quasiquote(const SExprPtr& x, int depth, Span ctx);
        SExprPtr make_quote(SExprPtr datum, Span s);

        // syntax-rules helpers
        ExpanderResult<SyntaxPatternPtr> parse_syntax_pattern(
            const SExprPtr& node,
            const std::unordered_set<std::string>& literals,
            const std::unordered_set<std::string>& bound_vars) const;

        ExpanderResult<SyntaxTemplatePtr> parse_syntax_template(
            const SExprPtr& node,
            const std::unordered_set<std::string>& pattern_vars) const;

        static void collect_pattern_vars(const SyntaxPattern& pat,
                                         std::unordered_set<std::string>& out);

        static bool match_pattern(const SyntaxPattern& pat,
                                  const SExprPtr& input,
                                  MatchEnv& env);

        ExpanderResult<SExprPtr> instantiate_template(
            const SyntaxTemplate& tmpl,
            const MatchEnv& env,
            std::unordered_map<std::string, std::string>& renames,
            Span ctx) const;

        ExpanderResult<SExprPtr> try_expand_macro(const std::string& name,
                                                   const List& lst);
    };

}

