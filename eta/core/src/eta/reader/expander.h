#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "eta/reader/parser.h"

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

    class Expander {
    public:
        explicit Expander(ExpanderConfig cfg = {});

        ExpanderResult<SExprPtr> expand_form(const SExprPtr& in);
        ExpanderResult<std::vector<SExprPtr>> expand_many(const std::vector<SExprPtr>& forms);

    private:
        ExpanderConfig cfg_{};
        std::size_t depth_{0};

        // Error helpers
        static ExpandError syntax_error(Span sp, std::string_view msg, std::string hint = {});
        static ExpandError arity_error(Span sp, std::string_view form, std::size_t expected, std::size_t got);
        static ExpandError invalid_syntax(Span sp, std::string_view form, std::string_view expected);

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

        //! Modules/directives
        ExpanderResult<SExprPtr> handle_module_list(const List& lst);    // (module name ...)
        ExpanderResult<SExprPtr> handle_export(const List& lst);
        ExpanderResult<SExprPtr> handle_import(const List& lst);

        // Convenience sugars explicitly requested
        ExpanderResult<SExprPtr> handle_def(const List& lst);   // (def ...) sugar → define
        ExpanderResult<SExprPtr> handle_defun(const List& lst); // (defun name (args) body...)

        // -- Helpers --
        static bool is_symbol_named(const SExprPtr& p, std::string_view name);
        static SExprPtr make_symbol(std::string name, Span s);
        static SExprPtr make_list(std::vector<SExprPtr> elems, Span s);
        static SExprPtr make_dotted_list(std::vector<SExprPtr> head, SExprPtr tail, Span s);

        static SExprPtr deep_clone(const SExpr& n);
        static SExprPtr deep_clone(const SExprPtr& p) { return p ? deep_clone(*p) : nullptr; }

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
    };

}