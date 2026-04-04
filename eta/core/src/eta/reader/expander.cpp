#include "expander.h"

#include <cassert>
#include <cstdint>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <random>

namespace eta::reader::expander {

    using namespace utils;

    // ---------- Reserved keywords ----------
    static const std::unordered_set<std::string>& reserved_keywords() {
        static const std::unordered_set<std::string> K = {
            // Core/derived
            "quote","if","begin","lambda","define","set!",
            "let","let*","letrec","cond","and","or",
            "quasiquote","unquote","unquote-splicing",
            // Modules/macros
            "module","import","export","define-syntax",
            // Advanced control / multiple values (future)
            "call-with-current-continuation","call/cc","dynamic-wind",
            "values","call-with-values",
            // Convenience (existing and new)
            "case","do","when","unless",
            "def","defun","progn",
            "define-record-type"
        };
        return K;
    }

    static bool is_reserved(std::string_view name) {
        return reserved_keywords().contains(std::string(name));
    }

    bool Expander::is_reserved(std::string_view name) { return expander::is_reserved(name); }

    // ---------- Utilities: constructors and cloning ----------
    SExprPtr Expander::make_symbol(std::string name, Span s) {
        auto p = std::make_unique<SExpr>();
        Symbol node; node.span = s; node.name = std::move(name);
        p->value = std::move(node);
        return p;
    }

    SExprPtr Expander::make_nil(Span s) {
        auto p = std::make_unique<SExpr>();
        parser::Nil node; node.span = s;
        p->value = std::move(node);
        return p;
    }

    SExprPtr Expander::make_list(std::vector<SExprPtr> elems, Span s) {
        auto p = std::make_unique<SExpr>();
        List l; l.span = s; l.dotted = false; l.elems = std::move(elems);
        p->value = std::move(l);
        return p;
    }

    SExprPtr Expander::make_dotted_list(std::vector<SExprPtr> head, SExprPtr tail, Span s) {
        auto p = std::make_unique<SExpr>();
        List l; l.span = s; l.dotted = true; l.elems = std::move(head); l.tail = std::move(tail);
        p->value = std::move(l);
        return p;
    }

    using parser::deep_copy;


    std::string Expander::gensym(const std::string& hint) {
        // Session-based gensym to avoid collisions
        static const std::uint64_t session = []{
            std::random_device rd; std::mt19937_64 gen(rd()); return gen();
        }();
        static std::atomic<std::uint64_t> counter{0};
        auto n = counter.fetch_add(1, std::memory_order_relaxed) + 1; // no wrap guard needed
        std::ostringstream oss; oss << hint << ":" << session << ":" << n;
        return oss.str();
    }

    // ---------- Quasiquote helpers (local) ----------
    namespace {
        enum class QqHead { None, Quote, Quasiquote, Unquote, UnquoteSplicing };

        inline QqHead list_head_qq(const SExprPtr& p, const List** outList,
                                   const SExprPtr** outArg) {
            if (!p || !p->is<List>()) return QqHead::None;
            const auto* lst = p->as<List>();
            if (lst->dotted || lst->elems.size() != 2) return QqHead::None;
            const auto* sym = lst->elems[0] && lst->elems[0]->is<Symbol>()
                              ? lst->elems[0]->as<Symbol>() : nullptr;
            if (!sym) return QqHead::None;
            const std::string& nm = sym->name;
            if (nm == "quote") { if (outList) *outList = lst; if (outArg) *outArg = &lst->elems[1]; return QqHead::Quote; }
            if (nm == "quasiquote") { if (outList) *outList = lst; if (outArg) *outArg = &lst->elems[1]; return QqHead::Quasiquote; }
            if (nm == "unquote") { if (outList) *outList = lst; if (outArg) *outArg = &lst->elems[1]; return QqHead::Unquote; }
            if (nm == "unquote-splicing") { if (outList) *outList = lst; if (outArg) *outArg = &lst->elems[1]; return QqHead::UnquoteSplicing; }
            return QqHead::None;
        }

        inline QqHead reader_qq(const SExprPtr& p, const ReaderForm** outRf,
                                 const SExprPtr** outArg) {
            if (!p || !p->is<ReaderForm>()) return QqHead::None;
            const auto* rf = p->as<ReaderForm>();
            if (outRf) *outRf = rf;
            if (outArg) *outArg = &rf->expr;
            switch (rf->kind) {
                case parser::QuoteKind::Quote:            return QqHead::Quote;
                case parser::QuoteKind::Quasiquote:       return QqHead::Quasiquote;
                case parser::QuoteKind::Unquote:          return QqHead::Unquote;
                case parser::QuoteKind::UnquoteSplicing:  return QqHead::UnquoteSplicing;
            }
            return QqHead::None;
        }
    } // anonymous namespace

    SExprPtr Expander::make_quote(SExprPtr datum, Span s) {
        std::vector<SExprPtr> v; v.reserve(2);
        v.push_back(make_symbol("quote", s));
        v.push_back(std::move(datum));
        return make_list(std::move(v), s);
    }

    // ---------- Constructor ----------
    Expander::Expander(ExpanderConfig cfg) : cfg_(cfg) {}

    // ---------- Public API ----------
    ExpanderResult<std::vector<SExprPtr>> Expander::expand_many(const std::vector<SExprPtr>& forms) {
        std::vector<SExprPtr> out; out.reserve(forms.size());
        for (const auto& f : forms) {
            auto r = expand_form(f);
            if (!r) return std::unexpected(r.error());
            out.push_back(std::move(*r));
        }
        return out;
    }

    ExpanderResult<SExprPtr> Expander::expand_form(const SExprPtr& in) {
        if (!in) return SExprPtr{}; // nothing
        // Depth guard to prevent runaway expansion
        struct Guard { std::size_t& d; ~Guard(){ --d; } } g{depth_};
        if (++depth_ > cfg_.depth_limit) {
            return std::unexpected(ExpandError{ExpandError::Kind::ExpansionDepthExceeded, in->span(), "expansion depth exceeded"});
        }

        // Reader forms: handle quote/quasiquote/unquote abbreviations
        if (in->is<ReaderForm>()) {
            const auto* rf = in->as<ReaderForm>();
            switch (rf->kind) {
                case parser::QuoteKind::Quote: {
                    // Desugar 'x → (quote x) so the SA sees a List, not a ReaderForm
                    std::vector<SExprPtr> elems;
                    elems.push_back(make_symbol("quote", rf->span));
                    elems.push_back(deep_clone(rf->expr));
                    return make_list(std::move(elems), rf->span);
                }
                case parser::QuoteKind::Quasiquote:
                    return expand_quasiquote(rf->expr, /*depth*/1, rf->span);
                case parser::QuoteKind::Unquote:
                case parser::QuoteKind::UnquoteSplicing:
                    return std::unexpected(syntax_error(rf->span, "unquote outside quasiquote"));
            }
        }
        if (in->is<List>()) {
            return expand_application(*in->as<List>());
        }
        if (in->is<ModuleForm>()) {
            // Normalize ModuleForm -> (module name (export ...) ...expanded body...)
            const auto* mf = in->as<ModuleForm>();
            std::vector<SExprPtr> elems;
            elems.push_back(make_symbol("module", mf->span));
            elems.push_back(make_symbol(mf->name, mf->span));
            if (!mf->exports.empty()) {
                std::vector<SExprPtr> ex;
                ex.push_back(make_symbol("export", mf->span));
                for (const auto& nm : mf->exports) ex.push_back(make_symbol(nm, mf->span));
                elems.push_back(make_list(std::move(ex), mf->span));
            }
            // Expand each body form; splice top-level (begin ...) per R7RS
            for (const auto& form : mf->body) {
                auto exp = expand_form(form);
                if (!exp) return std::unexpected(exp.error());
                if (auto* bl = exp->get()->as<List>();
                    bl && !bl->elems.empty() && is_symbol_named(bl->elems[0], "begin")) {
                    for (size_t j = 1; j < bl->elems.size(); ++j)
                        elems.push_back(deep_clone(bl->elems[j]));
                } else {
                    elems.push_back(std::move(*exp));
                }
            }
            return make_list(std::move(elems), mf->span);
        }

        // Atoms: clone
        return deep_clone(in);
    }

    // ---------- Error helpers and shared utilities ----------
    ExpandError Expander::syntax_error(Span sp, std::string_view msg, std::string hint) {
        ExpandError e; e.kind = ExpandError::Kind::InvalidSyntax; e.span = sp; e.message = std::string(msg);
        if (!hint.empty()) { e.message += ": "; e.message += hint; }
        return e;
    }
    ExpandError Expander::arity_error(Span sp, std::string_view form, std::size_t expected, std::size_t got) {
        std::ostringstream oss; oss << form << ": expected " << expected << " argument(s); got " << got;
        return ExpandError{ExpandError::Kind::ArityError, sp, oss.str()};
    }
    ExpandError Expander::invalid_syntax(Span sp, std::string_view form, std::string_view expected) {
        std::ostringstream oss; oss << form << ": invalid syntax; " << expected;
        return ExpandError{ExpandError::Kind::InvalidSyntax, sp, oss.str()};
    }

    ExpanderResult<void> Expander::validate_identifier(
        const std::string& name, Span span,
        std::unordered_set<std::string>* seen,
        std::string_view context) {
        if (is_reserved(name)) {
            return std::unexpected(ExpandError{
                ExpandError::Kind::ReservedKeyword, span,
                std::string("reserved keyword as ") + std::string(context) + ": " + name});
        }
        if (seen && !seen->insert(name).second) {
            return std::unexpected(ExpandError{
                ExpandError::Kind::DuplicateIdentifier, span,
                std::string("duplicate ") + std::string(context) + ": " + name});
        }
        return {};
    }

    ExpanderResult<std::vector<SExprPtr>> Expander::expand_list_elems(const std::vector<SExprPtr>& elems) const {
        std::vector<SExprPtr> xs; xs.reserve(elems.size());
        for (const auto& a : elems) {
            if (!a) return std::unexpected(syntax_error(Span{}, "non-null datum expected"));
            auto r = const_cast<Expander*>(this)->expand_form(a);
            if (!r) return std::unexpected(r.error());
            xs.push_back(std::move(*r));
        }
        return xs;
    }

    ExpanderResult<SExprPtr> Expander::expand_application(const List& lst) {
        using Handler = ExpanderResult<SExprPtr>(Expander::*)(const List&);
        static const std::unordered_map<std::string_view, Handler> kSpecials = {
            {"quote", &Expander::handle_quote_like},
            {"quasiquote", &Expander::handle_quote_like},
            {"unquote", &Expander::handle_quote_like},
            {"unquote-splicing", &Expander::handle_quote_like},
            {"if", &Expander::handle_if},
            {"begin", &Expander::handle_begin},
            {"lambda", &Expander::handle_lambda},
            {"define", &Expander::handle_define},
            {"set!", &Expander::handle_set_bang},
            {"let", &Expander::handle_let},
            {"let*", &Expander::handle_let_star},
            {"letrec", &Expander::handle_letrec},
            {"letrec*", &Expander::handle_letrec_star},
            {"cond", &Expander::handle_cond},
            {"case", &Expander::handle_case},
            {"and", &Expander::handle_and},
            {"or", &Expander::handle_or},
            {"when", &Expander::handle_when},
            {"unless", &Expander::handle_unless},
            {"do", &Expander::handle_do},
            {"module", &Expander::handle_module_list},
            {"export", &Expander::handle_export},
            {"import", &Expander::handle_import},
            // convenience
            {"def", &Expander::handle_def},
            {"defun", &Expander::handle_defun},
            {"progn", &Expander::handle_begin},
            // records
            {"define-record-type", &Expander::handle_define_record_type},
            // macros
            {"define-syntax", &Expander::handle_define_syntax},
        };

        if (lst.elems.empty()) {
            return make_list({}, lst.span);
        }

        const auto* headSym = lst.elems[0] ? lst.elems[0]->as<Symbol>() : nullptr;
        if (headSym) {
            auto it = kSpecials.find(std::string_view(headSym->name));
            if (it != kSpecials.end()) {
                return (this->*(it->second))(lst);
            }
            // Check user-defined macros
            auto macro_it = macro_env_.find(headSym->name);
            if (macro_it != macro_env_.end()) {
                return try_expand_macro(headSym->name, lst);
            }
        }
        // General application: proper list required
        if (lst.dotted) {
            return std::unexpected(invalid_syntax(lst.span, "application", "proper list required; dotted lists are not allowed"));
        }
        auto xs = expand_list_elems(lst.elems);
        if (!xs) return std::unexpected(xs.error());
        return make_list(std::move(*xs), lst.span);
    }

    // ---------- Quasiquote main expansion ----------
    ExpanderResult<SExprPtr> Expander::expand_quasiquote(const SExprPtr& x, int depth, Span ctx) {
        const ReaderForm* rf = nullptr; const List* lst = nullptr; const SExprPtr* arg = nullptr;
        QqHead h = reader_qq(x, &rf, &arg);
        if (h == QqHead::None) h = list_head_qq(x, &lst, &arg);

        auto quote_datum = [&](const SExprPtr& d) { return make_quote(deep_clone(d), d ? d->span() : ctx); };

        switch (h) {
            case QqHead::Quote:
                return deep_clone(x);
            case QqHead::Quasiquote:
                return expand_quasiquote(*arg, depth + 1, (*arg) ? (*arg)->span() : ctx);
            case QqHead::Unquote:
                if (depth == 0) return std::unexpected(syntax_error(ctx, "unquote outside quasiquote"));
                if (depth == 1) {
                    return expand_form(*arg);
                } else {
                    std::vector<SExprPtr> v; v.reserve(2);
                    v.push_back(make_symbol("unquote", (*arg) ? (*arg)->span() : ctx));
                    auto inner = expand_quasiquote(*arg, depth - 1, (*arg) ? (*arg)->span() : ctx);
                    if (!inner) return std::unexpected(inner.error());
                    v.push_back(std::move(*inner));
                    return make_list(std::move(v), ctx);
                }
            case QqHead::UnquoteSplicing:
                if (depth == 0) return std::unexpected(syntax_error(ctx, "unquote-splicing outside quasiquote"));
                if (depth > 1) {
                    std::vector<SExprPtr> v; v.reserve(2);
                    v.push_back(make_symbol("unquote-splicing", (*arg) ? (*arg)->span() : ctx));
                    auto inner = expand_quasiquote(*arg, depth - 1, (*arg) ? (*arg)->span() : ctx);
                    if (!inner) return std::unexpected(inner.error());
                    v.push_back(std::move(*inner));
                    return make_list(std::move(v), ctx);
                }
                return std::unexpected(syntax_error(ctx, "unquote-splicing is only valid in list/vector element position"));
            case QqHead::None:
                break;
        }

        if (!x) return SExprPtr{};

        if (x->is<List>()) {
            const auto& L = *x->as<List>();
            SExprPtr rest;
            if (!L.dotted) {
                auto nilList = make_list({}, L.span);
                rest = make_quote(std::move(nilList), L.span);
            } else {
                const ReaderForm* trf=nullptr; const List* tlst=nullptr; const SExprPtr* targ=nullptr;
                QqHead th = reader_qq(L.tail, &trf, &targ);
                if (th == QqHead::None) th = list_head_qq(L.tail, &tlst, &targ);
                if (th == QqHead::UnquoteSplicing && depth == 1)
                    return std::unexpected(syntax_error(L.tail ? L.tail->span() : L.span, "unquote-splicing not allowed in dotted tail"));
                auto tailExp = expand_quasiquote(L.tail, depth, L.tail ? L.tail->span() : L.span);
                if (!tailExp) return std::unexpected(tailExp.error());
                rest = std::move(*tailExp);
            }

            auto make_cons = [&](SExprPtr a, SExprPtr d, Span s) {
                std::vector<SExprPtr> v; v.reserve(3);
                v.push_back(make_symbol("cons", s));
                v.push_back(std::move(a));
                v.push_back(std::move(d));
                return make_list(std::move(v), s);
            };
            auto make_append2 = [&](SExprPtr a, SExprPtr b, Span s) {
                std::vector<SExprPtr> v; v.reserve(3);
                v.push_back(make_symbol("append", s));
                v.push_back(std::move(a));
                v.push_back(std::move(b));
                return make_list(std::move(v), s);
            };

            for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(L.elems.size()) - 1; i >= 0; --i) {
                const auto& e = L.elems[static_cast<size_t>(i)];
                const ReaderForm* erf=nullptr; const List* elst=nullptr; const SExprPtr* earg=nullptr;
                QqHead eh = reader_qq(e, &erf, &earg);
                if (eh == QqHead::None) eh = list_head_qq(e, &elst, &earg);

                if (eh == QqHead::UnquoteSplicing && depth == 1) {
                    auto inner = expand_form(*earg); if (!inner) return std::unexpected(inner.error());
                    rest = make_append2(std::move(*inner), std::move(rest), e ? e->span() : L.span);
                    continue;
                }

                auto elem = expand_quasiquote(e, depth, e ? e->span() : L.span);
                if (!elem) return std::unexpected(elem.error());
                rest = make_cons(std::move(*elem), std::move(rest), e ? e->span() : L.span);
            }
            return rest;
        }

        if (x->is<Vector>()) {
            const auto& V = *x->as<Vector>();
            // Expand vector elements by constructing a list with the same elements
            std::vector<SExprPtr> elems; elems.reserve(V.elems.size());
            for (const auto& e : V.elems) elems.push_back(deep_clone(e));
            auto listNode = make_list(std::move(elems), V.span);
            auto listExp = expand_quasiquote(listNode, depth, V.span); if (!listExp) return std::unexpected(listExp.error());
            std::vector<SExprPtr> call; call.reserve(2);
            call.push_back(make_symbol("list->vector", V.span));
            call.push_back(std::move(*listExp));
            return make_list(std::move(call), V.span);
        }

        if (x->is<ByteVector>()) {
            return quote_datum(x);
        }

        return quote_datum(x);
    }

    // ---------- Handlers ----------
    ExpanderResult<SExprPtr> Expander::handle_quote_like(const List& lst) {
        if (lst.elems.size() != 2) {
            const auto* s = lst.elems[0] && lst.elems[0]->is<Symbol>() ? lst.elems[0]->as<Symbol>() : nullptr;
            std::string_view form = s ? std::string_view(s->name) : std::string_view("quote");
            return std::unexpected(arity_error(lst.span, form, 1, lst.elems.size() - 1));
        }
        const auto* sym = lst.elems[0] && lst.elems[0]->is<Symbol>() ? lst.elems[0]->as<Symbol>() : nullptr;
        if (!sym) return std::unexpected(syntax_error(lst.span, "invalid quote-like head"));
        if (sym->name == "quote") {
            std::vector<SExprPtr> v; v.reserve(2);
            v.push_back(make_symbol("quote", lst.span));
            v.push_back(deep_clone(lst.elems[1]));
            return make_list(std::move(v), lst.span);
        }
        if (sym->name == "quasiquote") {
            return expand_quasiquote(lst.elems[1], /*depth*/1, lst.span);
        }
        if (sym->name == "unquote" || sym->name == "unquote-splicing") {
            return std::unexpected(syntax_error(lst.span, std::string(sym->name) + " outside quasiquote"));
        }
        return std::unexpected(syntax_error(lst.span, "unknown quote-like form"));
    }

    ExpanderResult<SExprPtr> Expander::handle_if(const List& lst) {
        if (lst.elems.size() < 3 || lst.elems.size() > 4)
            return std::unexpected(invalid_syntax(lst.span, "if", "expected (if test consequent [alternate])"));
        std::vector<SExprPtr> xs; xs.reserve(4);
        xs.push_back(deep_clone(lst.elems[0]));
        for (size_t i=1;i<lst.elems.size();++i) {
            auto r = expand_form(lst.elems[i]); if (!r) return std::unexpected(r.error()); xs.push_back(std::move(*r));
        }
        // If no alternate branch, fill in (begin) to satisfy semantic analyzer
        if (xs.size() == 3) {
            xs.push_back(make_begin(lst.span, {}));
        }
        return make_list(std::move(xs), lst.span);
    }

    ExpanderResult<SExprPtr> Expander::handle_begin(const List& lst) {
        std::vector<SExprPtr> tail;
        tail.reserve(lst.elems.size() - 1);
        for (auto it = lst.elems.begin() + 1; it != lst.elems.end(); ++it) {
            tail.push_back(deep_clone(*it));
        }
        auto xs = expand_list_elems(tail);
        if (!xs) return std::unexpected(xs.error());
        std::vector<SExprPtr> out; out.reserve(xs->size() + 1);
        out.push_back(deep_clone(lst.elems[0]));
        for (auto& e : *xs) out.push_back(std::move(e));
        return make_list(std::move(out), lst.span);
    }

    ExpanderResult<SExprPtr> Expander::handle_define(const List& lst) {
        if (lst.elems.size() < 3)
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "define expects a name and a value"});

        // (define id expr)
        if (lst.elems[1] && lst.elems[1]->is<Symbol>()) {
            const auto* s = lst.elems[1]->as<Symbol>();
            if (is_reserved(s->name))
                return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, lst.elems[1]->span(), "cannot define reserved keyword: "+s->name});

            auto rhs = expand_form(lst.elems[2]); if (!rhs) return std::unexpected(rhs.error());
            return make_form(lst.span, "define", deep_clone(lst.elems[1]), std::move(*rhs));
        }

        // (define (f args...) body...)
        if (!lst.elems[1] || !lst.elems[1]->is<List>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.elems[1] ? lst.elems[1]->span() : lst.span, "bad define head"});

        const auto& sig = *lst.elems[1]->as<List>();
        if (sig.elems.empty() || !sig.elems[0] || !sig.elems[0]->is<Symbol>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, sig.span, "bad define head"});

        const auto* nameSym = sig.elems[0]->as<Symbol>();
        if (is_reserved(nameSym->name))
            return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, nameSym->span, "cannot define reserved keyword: "+nameSym->name});

        // Build a formal node by dropping the first symbol from the signature
        SExprPtr formalsNode;
        if (sig.elems.size() == 1 && sig.dotted && sig.tail && sig.tail->is<Symbol>()) {
            // (define (f . rest) ...) => formals is symbol 'rest'
            formalsNode = deep_clone(sig.tail);
        } else {
            std::vector<SExprPtr> params;
            for (size_t i=1;i<sig.elems.size();++i) params.push_back(deep_clone(sig.elems[i]));
            if (sig.dotted && sig.tail) {
                formalsNode = make_dotted_list(std::move(params), deep_clone(sig.tail), sig.span);
            } else {
                formalsNode = make_list(std::move(params), sig.span);
            }
        }

        // Build lambda: (lambda formals body...)
        std::vector<SExprPtr> bodyForms;
        for (size_t i=2;i<lst.elems.size();++i) bodyForms.push_back(deep_clone(lst.elems[i]));
        auto lamE = make_lambda(lst.span, std::move(formalsNode), std::move(bodyForms));

        // Normalize lambda (validates formals and expands body)
        auto lamX = handle_lambda(*lamE->as<List>());
        if (!lamX) return std::unexpected(lamX.error());

        return make_form(lst.span, "define", make_symbol(nameSym->name, nameSym->span), std::move(*lamX));
    }

    ExpanderResult<SExprPtr> Expander::handle_set_bang(const List& lst) {
        if (lst.elems.size() != 3 || !lst.elems[1] || !lst.elems[1]->is<Symbol>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "set! expects (set! id expr)"});
        auto rhs = expand_form(lst.elems[2]); if (!rhs) return std::unexpected(rhs.error());
        return make_set(lst.span, deep_clone(lst.elems[1]), std::move(*rhs));
    }

    ExpanderResult<Formals> Expander::parse_formals(const SExprPtr& node) const {
        Formals f; f.span = node->span();
        std::unordered_set<std::string> seen;

        if (node->is<Symbol>()) {
            const auto* s = node->as<Symbol>();
            if (auto err = validate_identifier(s->name, s->span, nullptr, "rest parameter"); !err) {
                return std::unexpected(err.error());
            }
            f.rest = s->name;
            return f;
        }
        if (!node->is<List>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, node->span(), "lambda formals must be a symbol or list"});

        const auto& lst = *node->as<List>();

        // Helper to validate and add a parameter
        auto add_param = [&](const SExprPtr& p, size_t i) -> ExpanderResult<void> {
            if (!p || !p->is<Symbol>()) {
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax,
                    p ? p->span() : lst.span,
                    "parameter at index " + std::to_string(i) + " must be a symbol"});
            }
            const auto* s = p->as<Symbol>();
            if (auto err = validate_identifier(s->name, s->span, &seen, "parameter"); !err) {
                return std::unexpected(err.error());
            }
            f.fixed.push_back(s->name);
            return {};
        };

        for (size_t i = 0; i < lst.elems.size(); ++i) {
            if (auto err = add_param(lst.elems[i], i); !err) {
                return std::unexpected(err.error());
            }
        }

        if (lst.dotted) {
            if (!lst.tail || !lst.tail->is<Symbol>())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "dotted formals: rest must be a symbol"});
            const auto* rest = lst.tail->as<Symbol>();
            if (auto err = validate_identifier(rest->name, rest->span, &seen, "parameter"); !err) {
                return std::unexpected(err.error());
            }
            f.rest = rest->name;
        }

        return f;
    }

    ExpanderResult<SExprPtr> Expander::handle_lambda(const List& lst) {
        if (lst.elems.size() < 3)
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "lambda requires at least 2 arguments: (lambda formals body...)"});

        auto fm = parse_formals(lst.elems[1]);
        if (!fm) return std::unexpected(fm.error());

        // Body clone for optional internal-define rewriting
        std::vector<SExprPtr> body;
        for (size_t i=2;i<lst.elems.size();++i) body.push_back(deep_clone(lst.elems[i]));

        if (cfg_.enable_internal_defines_to_letrec) {
            auto rew = rewrite_internal_defines_to_letrec(body);
            if (!rew) return std::unexpected(rew.error());
            if (*rew) {
                // Created a (letrec ...) wrapper in body[0]
                std::vector<SExprPtr> lam; lam.push_back(make_symbol("lambda", lst.span));
                lam.push_back(deep_clone(lst.elems[1])); // original formals
                lam.push_back(std::move(body[0]));
                auto lamE = make_list(std::move(lam), lst.span);

                // Expand the wrapped letrec body
                std::vector<SExprPtr> finalv; finalv.reserve(3);
                finalv.push_back(make_symbol("lambda", lst.span));
                finalv.push_back(deep_clone(lst.elems[1]));
                // expand_form on the wrapper's body position
                auto inner = expand_form(lamE->as<List>()->elems[2]);
                if (!inner) return std::unexpected(inner.error());
                finalv.push_back(std::move(*inner));
                return make_list(std::move(finalv), lst.span);
            }
        }

        // Normal path: expand each body form
        std::vector<SExprPtr> out; out.reserve(lst.elems.size());
        out.push_back(make_symbol("lambda", lst.span));
        out.push_back(deep_clone(lst.elems[1])); // keep original formal node (already validated)
        for (auto& b : body) { auto r = expand_form(b); if (!r) return std::unexpected(r.error()); out.push_back(std::move(*r)); }
        return make_list(std::move(out), lst.span);
    }

    ExpanderResult<std::vector<std::pair<SExprPtr, SExprPtr>>>
    Expander::parse_let_pairs(const List& pair_list, bool require_unique_names) const {
        std::vector<std::pair<SExprPtr,SExprPtr>> out;
        std::unordered_set<std::string> names;
        std::unordered_set<std::string>* seen_ptr = require_unique_names ? &names : nullptr;

        for (const auto& p : pair_list.elems) {
            if (!p || !p->is<List>())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidLetBindings, pair_list.span, "let binding must be (id expr)"});
            const auto& pr = *p->as<List>();
            if (pr.dotted || pr.elems.size() != 2 || !pr.elems[0] || !pr.elems[0]->is<Symbol>())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidLetBindings, pr.span, "let binding must be (id expr)"});
            const auto* id = pr.elems[0]->as<Symbol>();

            if (auto err = validate_identifier(id->name, id->span, seen_ptr, "binding"); !err) {
                // Convert to InvalidLetBindings kind for consistency
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidLetBindings, err.error().span, err.error().message});
            }

            out.emplace_back(make_symbol(id->name, id->span), deep_clone(pr.elems[1]));
        }
        return out;
    }

    bool Expander::is_named_let_syntax(const List& lst) {
        return lst.elems.size() >= 3 && lst.elems[1] && lst.elems[1]->is<Symbol>() && lst.elems[2] && lst.elems[2]->is<List>();
    }

    ExpanderResult<SExprPtr> Expander::handle_let(const List& lst) {
        // Named let
        if (is_named_let_syntax(lst)) {
            const auto* nameSym = lst.elems[1]->as<Symbol>();
            const auto& pairs = *lst.elems[2]->as<List>();
            auto parsed = parse_let_pairs(pairs, /*unique*/true); if (!parsed) return std::unexpected(parsed.error());

            // Prevent shadowing of the recursive name by a parameter
            for (const auto& pr : *parsed) {
                const auto* idSym = pr.first->as<Symbol>();
                if (idSym && idSym->name == nameSym->name) {
                    return std::unexpected(ExpandError{ExpandError::Kind::InvalidLetBindings, idSym->span, "named let parameter shadows function name: "+idSym->name});
                }
            }

            // Build lambda for the function
            std::vector<SExprPtr> params; params.reserve(parsed->size());
            std::vector<SExprPtr> inits;  inits.reserve(parsed->size());
            for (auto& [id, expr] : *parsed) {
                params.push_back(std::move(id));
                auto r = expand_form(expr); if (!r) return std::unexpected(r.error());
                inits.push_back(std::move(*r));
            }

            std::vector<SExprPtr> lam; lam.push_back(make_symbol("lambda", lst.span));
            auto formals = make_list({}, lst.span);
            formals->as<List>()->elems = std::move(params);
            lam.push_back(std::move(formals));
            for (size_t i=3;i<lst.elems.size();++i) lam.push_back(deep_clone(lst.elems[i]));

            auto lamE = make_list(std::move(lam), lst.span);
            auto lamX = handle_lambda(*lamE->as<List>()); if (!lamX) return std::unexpected(lamX.error());

            auto bindingPair = make_list({}, lst.span);
            {
                auto& v = bindingPair->as<List>()->elems;
                v.push_back(make_symbol(nameSym->name, nameSym->span));
                v.push_back(std::move(*lamX));
            }
            auto bindingList = make_list({}, lst.span);
            bindingList->as<List>()->elems.push_back(std::move(bindingPair));

            std::vector<SExprPtr> call;
            call.push_back(make_symbol(nameSym->name, nameSym->span));
            for (auto& e : inits) call.push_back(std::move(e));
            auto callList = make_list(std::move(call), lst.span);

            std::vector<SExprPtr> letrecForm;
            letrecForm.push_back(make_symbol("letrec", lst.span));
            letrecForm.push_back(std::move(bindingList));
            letrecForm.push_back(std::move(callList));
            auto letrecExpr = make_list(std::move(letrecForm), lst.span);
            return handle_letrec(*letrecExpr->as<List>());
        }

        // Regular let: (let ((x e) ...) body...)
        if (lst.elems.size() < 3 || !lst.elems[1] || !lst.elems[1]->is<List>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "let expects bindings and body"});

        const auto& pairs = *lst.elems[1]->as<List>();
        auto parsed = parse_let_pairs(pairs, /*unique*/true); if (!parsed) return std::unexpected(parsed.error());

        std::vector<SExprPtr> params; params.reserve(parsed->size());
        std::vector<SExprPtr> inits;  inits.reserve(parsed->size());
        for (auto& [id, expr] : *parsed) { params.push_back(std::move(id)); auto r = expand_form(expr); if (!r) return std::unexpected(r.error()); inits.push_back(std::move(*r)); }

        // ((lambda (x ...) body...) e ...)
        std::vector<SExprPtr> lam; lam.push_back(make_symbol("lambda", lst.span));
        auto formalList = make_list({}, pairs.span);
        formalList->as<List>()->elems = std::move(params);
        lam.push_back(std::move(formalList));
        for (size_t i=2;i<lst.elems.size();++i) { auto r = expand_form(lst.elems[i]); if (!r) return std::unexpected(r.error()); lam.push_back(std::move(*r)); }
        auto lamE = make_list(std::move(lam), lst.span);
        auto lamX = handle_lambda(*lamE->as<List>()); if (!lamX) return std::unexpected(lamX.error());

        std::vector<SExprPtr> app; app.push_back(std::move(*lamX));
        for (auto& e : inits) app.push_back(std::move(e));
        return make_list(std::move(app), lst.span);
    }

    ExpanderResult<SExprPtr> Expander::handle_let_star(const List& lst) {
        if (lst.elems.size() < 3 || !lst.elems[1] || !lst.elems[1]->is<List>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "let* expects bindings and body"});
        const auto& pairs = *lst.elems[1]->as<List>();

        if (pairs.elems.empty()) {
            // (let* () body...) == (let () body...)
            auto rebuilt = build_list(lst.span, make_symbol("let", lst.span), make_list({}, pairs.span));
            for (size_t i = 2; i < lst.elems.size(); ++i) {
                rebuilt->as<List>()->elems.push_back(deep_clone(lst.elems[i]));
            }
            return handle_let(*rebuilt->as<List>());
        }

        // Nest lets: (let ((x e)) (let* ((y f) ...) body...))
        const auto& firstPair = *pairs.elems[0]->as<List>();
        auto outerBinding = build_list(firstPair.span,
            deep_clone(firstPair.elems[0]),
            deep_clone(firstPair.elems[1]));
        auto outerBindings = build_list(pairs.span, std::move(outerBinding));

        // Build inner let* with remaining pairs
        auto restPairsList = make_list({}, pairs.span);
        for (size_t i = 1; i < pairs.elems.size(); ++i) {
            restPairsList->as<List>()->elems.push_back(deep_clone(pairs.elems[i]));
        }

        auto innerList = build_list(lst.span, make_symbol("let*", lst.span), std::move(restPairsList));
        for (size_t i = 2; i < lst.elems.size(); ++i) {
            innerList->as<List>()->elems.push_back(deep_clone(lst.elems[i]));
        }

        auto outerNode = build_list(lst.span,
            make_symbol("let", lst.span),
            std::move(outerBindings),
            std::move(innerList));
        return handle_let(*outerNode->as<List>());
    }

    ExpanderResult<SExprPtr> Expander::handle_letrec(const List& lst) {
        if (lst.elems.size() < 3 || !lst.elems[1] || !lst.elems[1]->is<List>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "letrec expects bindings and body"});
        const auto& pairs = *lst.elems[1]->as<List>();
        auto parsed = parse_let_pairs(pairs, /*unique*/true);
        if (!parsed) return std::unexpected(parsed.error());

        // Strategy: allocate placeholders with (let ((x '()) ...)) then (set! x e) ... body
        std::vector<std::pair<SExprPtr, SExprPtr>> placeholders;
        for (auto& [id, expr] : *parsed) {
            placeholders.emplace_back(deep_clone(id), make_nil(id->span()));
        }

        std::vector<SExprPtr> body;
        for (auto& [id, expr] : *parsed) {
            auto ex = expand_form(expr);
            if (!ex) return std::unexpected(ex.error());
            body.push_back(make_set(id->span(), deep_clone(id), std::move(*ex)));
        }
        for (size_t i = 2; i < lst.elems.size(); ++i) {
            auto r = expand_form(lst.elems[i]);
            if (!r) return std::unexpected(r.error());
            body.push_back(std::move(*r));
        }

        auto letBody = make_begin(lst.span, std::move(body));
        std::vector<SExprPtr> letBodyVec;
        letBodyVec.push_back(std::move(letBody));
        auto outerList = make_let(lst.span, std::move(placeholders), std::move(letBodyVec));
        return handle_let(*outerList->as<List>());
    }

    ExpanderResult<SExprPtr> Expander::handle_letrec_star(const List& lst) {
        // (letrec* ((x e) (y e) ...) body...)
        // Desugars to nested letrec forms for sequential initialization semantics:
        // (letrec ((x ex)) (letrec ((y ey)) ... body...))

        if (lst.elems.size() < 3 || !lst.elems[1] || !lst.elems[1]->is<List>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "letrec* expects bindings and body"});

        const auto& pairs = *lst.elems[1]->as<List>();

        // No bindings: same as (let () body...)
        if (pairs.elems.empty()) {
            return handle_let(*build_list(lst.span,
                make_symbol("let", lst.span),
                make_list({}, pairs.span),
                [&]{ std::vector<SExprPtr> body; for (size_t i = 2; i < lst.elems.size(); ++i) body.push_back(deep_clone(lst.elems[i])); return body.size() == 1 ? std::move(body[0]) : build_list(lst.span, make_symbol("begin", lst.span)); }()
            )->as<List>());
        }

        // Validate pairs
        auto parsed = parse_let_pairs(pairs, /*require_unique_names*/true);
        if (!parsed) return std::unexpected(parsed.error());

        // Build iteratively from the innermost (body) outward
        // Start with the body as a begin expression
        SExprPtr result = build_list(lst.span, make_symbol("begin", lst.span));
        for (size_t i = 2; i < lst.elems.size(); ++i) {
            result->as<List>()->elems.push_back(deep_clone(lst.elems[i]));
        }

        // Wrap with letrec forms from last binding to first
        for (auto it = parsed->rbegin(); it != parsed->rend(); ++it) {
            auto& [id, expr] = *it;

            // Build (letrec ((id expr)) result)
            auto bindingPair = build_list(id->span(), deep_clone(id), deep_clone(expr));
            auto bindingList = build_list(pairs.span, std::move(bindingPair));

            auto letrecForm = build_list(lst.span,
                make_symbol("letrec", lst.span),
                std::move(bindingList),
                std::move(result));

            // Expand this letrec and use result for next iteration
            auto expanded = handle_letrec(*letrecForm->as<List>());
            if (!expanded) return std::unexpected(expanded.error());
            result = std::move(*expanded);
        }

        return result;
    }

    ExpanderResult<SExprPtr> Expander::handle_cond(const List& lst) {
        if (lst.elems.size() < 2)
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "cond expects at least one clause"});

        // Build from the end forward to avoid recursion depth issues
        // Initial "rest" is an empty (begin)
        SExprPtr rest = build_list(lst.span, make_symbol("begin", lst.span));
        bool seenElse = false;

        for (std::ptrdiff_t idx = static_cast<std::ptrdiff_t>(lst.elems.size()) - 1; idx >= 1; --idx) {
            const auto& node = lst.elems[static_cast<size_t>(idx)];
            if (!node || !node->is<List>())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, node ? node->span() : lst.span, "cond clause must be a list"});
            const auto& clause = *node->as<List>();
            if (clause.elems.empty())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, clause.span, "empty cond clause"});

            // else-clause handling
            if (is_symbol_named(clause.elems[0], "else")) {
                if (static_cast<size_t>(idx) + 1 != lst.elems.size())
                    return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, clause.span, "else must be the last clause"});
                if (clause.elems.size() < 2)
                    return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, clause.span, "else must have at least one body expression"});
                std::vector<SExprPtr> bodyExprs;
                for (size_t k = 1; k < clause.elems.size(); ++k) {
                    auto r = expand_form(clause.elems[k]);
                    if (!r) return std::unexpected(r.error());
                    bodyExprs.push_back(std::move(*r));
                }
                rest = make_begin(clause.span, std::move(bodyExprs));
                seenElse = true;
                continue;
            }

            auto testX = expand_form(clause.elems[0]);
            if (!testX) return std::unexpected(testX.error());

            // Arrow clause: (test => proc)
            if (clause.elems.size() == 3 && is_symbol_named(clause.elems[1], "=>")) {
                auto procX = expand_form(clause.elems[2]);
                if (!procX) return std::unexpected(procX.error());
                auto t = gensym("t");

                auto letBindings = build_list(clause.span,
                    build_list(clause.span, make_symbol(t, clause.span), std::move(*testX)));
                auto call = build_list(clause.span, std::move(*procX), make_symbol(t, clause.span));
                auto ifnode = build_list(clause.span,
                    make_symbol("if", clause.span),
                    make_symbol(t, clause.span),
                    std::move(call),
                    deep_clone(rest));
                rest = build_list(clause.span,
                    make_symbol("let", clause.span),
                    std::move(letBindings),
                    std::move(ifnode));
                continue;
            }

            // Standard clause: (test expr...)
            if (clause.elems.size() == 1) {
                // (if test test rest)
                rest = make_if(clause.span, std::move(*testX), deep_clone(clause.elems[0]), std::move(rest));
                continue;
            }

            std::vector<SExprPtr> bodyExprs;
            for (size_t k = 1; k < clause.elems.size(); ++k) {
                auto r = expand_form(clause.elems[k]);
                if (!r) return std::unexpected(r.error());
                bodyExprs.push_back(std::move(*r));
            }
            rest = make_if(clause.span, std::move(*testX), make_begin(clause.span, std::move(bodyExprs)), std::move(rest));
        }

        return rest;
    }

    ExpanderResult<SExprPtr> Expander::handle_case(const List& lst) {
        // (case key-expr ((datum ...) body...) ... (else body...))
        // Desugar to: (let ((tmp key-expr))
        //               (if (or (eqv? tmp 'd1) (eqv? tmp 'd2) ...)
        //                   (begin body...)
        //                   (if ... else-body)))
        if (lst.elems.size() < 2)
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "case requires a key expression"});

        auto keyX = expand_form(lst.elems[1]);
        if (!keyX) return std::unexpected(keyX.error());

        auto tmpName = gensym("case-key");

        // Build nested if structure from the end backward (like cond)
        SExprPtr rest = build_list(lst.span, make_symbol("begin", lst.span));

        for (std::ptrdiff_t idx = static_cast<std::ptrdiff_t>(lst.elems.size()) - 1; idx >= 2; --idx) {
            const auto& clause = lst.elems[static_cast<size_t>(idx)];
            if (!clause || !clause->is<List>())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, clause ? clause->span() : lst.span, "case clause must be a list"});
            const auto& cl = *clause->as<List>();
            if (cl.elems.empty())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, cl.span, "empty case clause"});

            // else clause
            if (is_symbol_named(cl.elems[0], "else")) {
                if (static_cast<size_t>(idx) + 1 != lst.elems.size())
                    return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, cl.span, "else must be the last clause"});
                if (cl.elems.size() < 2)
                    return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, cl.span, "else must have at least one body expression"});
                std::vector<SExprPtr> bodyExprs;
                for (size_t k = 1; k < cl.elems.size(); ++k) {
                    auto r = expand_form(cl.elems[k]);
                    if (!r) return std::unexpected(r.error());
                    bodyExprs.push_back(std::move(*r));
                }
                rest = make_begin(cl.span, std::move(bodyExprs));
                continue;
            }

            // Normal clause: ((datum ...) body...)
            if (!cl.elems[0] || !cl.elems[0]->is<List>())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, cl.elems[0] ? cl.elems[0]->span() : cl.span, "case clause datums must be a list"});

            const auto& datums = *cl.elems[0]->as<List>();
            if (datums.elems.empty())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, datums.span, "case clause requires at least one datum"});

            // Build test: (or (eqv? tmp d1) (eqv? tmp d2) ...)
            // If only one datum, just use (eqv? tmp d)
            SExprPtr test;
            if (datums.elems.size() == 1) {
                test = build_list(datums.span,
                    make_symbol("eqv?", datums.span),
                    make_symbol(tmpName, datums.span),
                    make_quote(deep_clone(datums.elems[0]), datums.elems[0]->span()));
            } else {
                auto orExpr = build_list(datums.span, make_symbol("or", datums.span));
                for (const auto& d : datums.elems) {
                    orExpr->as<List>()->elems.push_back(make_form(d->span(), "eqv?",
                        make_symbol(tmpName, d->span()),
                        make_quote(deep_clone(d), d->span())));
                }
                test = std::move(orExpr);
            }

            // Build body: (begin body...)
            std::vector<SExprPtr> bodyExprs;
            for (size_t k = 1; k < cl.elems.size(); ++k) {
                auto r = expand_form(cl.elems[k]);
                if (!r) return std::unexpected(r.error());
                bodyExprs.push_back(std::move(*r));
            }

            // Build if: (if test body rest)
            rest = make_if(cl.span, std::move(test), make_begin(cl.span, std::move(bodyExprs)), std::move(rest));
        }

        // Wrap in let: (let ((tmp key-expr)) rest)
        std::vector<std::pair<SExprPtr, SExprPtr>> bindings;
        bindings.emplace_back(make_symbol(tmpName, lst.span), std::move(*keyX));
        std::vector<SExprPtr> letBodyVec;
        letBodyVec.push_back(std::move(rest));
        auto letExpr = make_let(lst.span, std::move(bindings), std::move(letBodyVec));

        return handle_let(*letExpr->as<List>());
    }

    // ---------- and / or / when / unless / do ----------

    ExpanderResult<SExprPtr> Expander::handle_and(const List& lst) {
        // (and) -> #t
        if (lst.elems.size() == 1) {
            auto p = std::make_unique<SExpr>();
            parser::Bool b; b.span = lst.span; b.value = true;
            p->value = std::move(b);
            return p;
        }
        // (and e) -> e
        if (lst.elems.size() == 2) {
            return expand_form(lst.elems[1]);
        }
        // (and e1 e2 ...) -> (if e1 (and e2 ...) #f)
        auto test = expand_form(lst.elems[1]);
        if (!test) return std::unexpected(test.error());

        // Build (and e2 ...)
        std::vector<SExprPtr> rest;
        rest.push_back(make_symbol("and", lst.span));
        for (size_t i = 2; i < lst.elems.size(); ++i) {
            rest.push_back(deep_clone(lst.elems[i]));
        }
        auto andRest = make_list(std::move(rest), lst.span);
        auto expandedRest = handle_and(*andRest->as<List>());
        if (!expandedRest) return std::unexpected(expandedRest.error());

        auto falseLit = std::make_unique<SExpr>();
        parser::Bool fb; fb.span = lst.span; fb.value = false;
        falseLit->value = std::move(fb);

        return make_if(lst.span, std::move(*test), std::move(*expandedRest), std::move(falseLit));
    }

    ExpanderResult<SExprPtr> Expander::handle_or(const List& lst) {
        // (or) -> #f
        if (lst.elems.size() == 1) {
            auto p = std::make_unique<SExpr>();
            parser::Bool b; b.span = lst.span; b.value = false;
            p->value = std::move(b);
            return p;
        }
        // (or e) -> e
        if (lst.elems.size() == 2) {
            return expand_form(lst.elems[1]);
        }
        // (or e1 e2 ...) -> (let ((t e1)) (if t t (or e2 ...)))
        auto init = expand_form(lst.elems[1]);
        if (!init) return std::unexpected(init.error());

        auto tmpName = gensym("or");

        // Build (or e2 ...)
        std::vector<SExprPtr> rest;
        rest.push_back(make_symbol("or", lst.span));
        for (size_t i = 2; i < lst.elems.size(); ++i) {
            rest.push_back(deep_clone(lst.elems[i]));
        }
        auto orRest = make_list(std::move(rest), lst.span);
        auto expandedRest = handle_or(*orRest->as<List>());
        if (!expandedRest) return std::unexpected(expandedRest.error());

        // (if t t (or e2 ...))
        auto ifExpr = make_if(lst.span,
            make_symbol(tmpName, lst.span),
            make_symbol(tmpName, lst.span),
            std::move(*expandedRest));

        // (let ((t e1)) ifExpr)
        std::vector<std::pair<SExprPtr, SExprPtr>> bindings;
        bindings.emplace_back(make_symbol(tmpName, lst.span), std::move(*init));
        std::vector<SExprPtr> letBody;
        letBody.push_back(std::move(ifExpr));
        auto letExpr = make_let(lst.span, std::move(bindings), std::move(letBody));
        return handle_let(*letExpr->as<List>());
    }

    ExpanderResult<SExprPtr> Expander::handle_when(const List& lst) {
        // (when test body...) -> (if test (begin body...) (begin))
        if (lst.elems.size() < 2)
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "when expects a test and body"});

        auto test = expand_form(lst.elems[1]);
        if (!test) return std::unexpected(test.error());

        std::vector<SExprPtr> bodyExprs;
        for (size_t i = 2; i < lst.elems.size(); ++i) {
            auto r = expand_form(lst.elems[i]);
            if (!r) return std::unexpected(r.error());
            bodyExprs.push_back(std::move(*r));
        }

        return make_if(lst.span,
            std::move(*test),
            make_begin(lst.span, std::move(bodyExprs)),
            make_begin(lst.span, {}));
    }

    ExpanderResult<SExprPtr> Expander::handle_unless(const List& lst) {
        // (unless test body...) -> (if test (begin) (begin body...))
        if (lst.elems.size() < 2)
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "unless expects a test and body"});

        auto test = expand_form(lst.elems[1]);
        if (!test) return std::unexpected(test.error());

        std::vector<SExprPtr> bodyExprs;
        for (size_t i = 2; i < lst.elems.size(); ++i) {
            auto r = expand_form(lst.elems[i]);
            if (!r) return std::unexpected(r.error());
            bodyExprs.push_back(std::move(*r));
        }

        return make_if(lst.span,
            std::move(*test),
            make_begin(lst.span, {}),
            make_begin(lst.span, std::move(bodyExprs)));
    }

    ExpanderResult<SExprPtr> Expander::handle_do(const List& lst) {
        // (do ((var init step) ...) (test expr...) body...)
        // Desugars to:
        //   (letrec ((loop (lambda (var ...)
        //     (if test (begin expr...) (begin body... (loop step...))))))
        //     (loop init...))
        if (lst.elems.size() < 3)
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "do expects variable clauses, test clause, and optional body"});
        if (!lst.elems[1] || !lst.elems[1]->is<List>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "do: variable clauses must be a list"});
        if (!lst.elems[2] || !lst.elems[2]->is<List>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "do: test clause must be a list"});

        const auto& varClauses = *lst.elems[1]->as<List>();
        const auto& testClause = *lst.elems[2]->as<List>();

        if (testClause.elems.empty())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, testClause.span, "do: test clause must not be empty"});

        // Parse variable clauses: ((var init step) ...) — step is optional (defaults to var)
        struct DoVar { std::string name; SExprPtr init; SExprPtr step; Span span; };
        std::vector<DoVar> vars;
        std::unordered_set<std::string> seen;
        for (const auto& vc : varClauses.elems) {
            if (!vc || !vc->is<List>())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, vc ? vc->span() : varClauses.span, "do: variable clause must be a list"});
            const auto& vcl = *vc->as<List>();
            if (vcl.elems.size() < 2 || vcl.elems.size() > 3)
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, vcl.span, "do: variable clause must be (var init) or (var init step)"});
            if (!vcl.elems[0] || !vcl.elems[0]->is<Symbol>())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, vcl.span, "do: variable name must be a symbol"});

            const auto* varSym = vcl.elems[0]->as<Symbol>();
            if (auto err = validate_identifier(varSym->name, varSym->span, &seen, "do variable"); !err)
                return std::unexpected(err.error());

            SExprPtr step = (vcl.elems.size() == 3) ? deep_clone(vcl.elems[2]) : make_symbol(varSym->name, varSym->span);
            vars.push_back(DoVar{varSym->name, deep_clone(vcl.elems[1]), std::move(step), vcl.span});
        }

        auto loopName = gensym("do-loop");

        // Build params list
        std::vector<SExprPtr> params;
        std::vector<SExprPtr> inits;
        std::vector<SExprPtr> steps;
        for (auto& v : vars) {
            params.push_back(make_symbol(v.name, v.span));
            inits.push_back(std::move(v.init));
            steps.push_back(std::move(v.step));
        }

        // Build test expression
        // (if test (begin expr...) (begin body... (loop step...)))
        auto testExpr = deep_clone(testClause.elems[0]);

        // Exit expressions (after test succeeds)
        std::vector<SExprPtr> exitExprs;
        for (size_t i = 1; i < testClause.elems.size(); ++i) {
            exitExprs.push_back(deep_clone(testClause.elems[i]));
        }

        // Body expressions (commands executed each iteration)
        std::vector<SExprPtr> bodyExprs;
        for (size_t i = 3; i < lst.elems.size(); ++i) {
            bodyExprs.push_back(deep_clone(lst.elems[i]));
        }

        // Build (loop step...)
        std::vector<SExprPtr> loopCall;
        loopCall.push_back(make_symbol(loopName, lst.span));
        for (auto& s : steps) loopCall.push_back(std::move(s));
        auto loopCallExpr = make_list(std::move(loopCall), lst.span);

        // Iteration body: (begin body... (loop step...))
        std::vector<SExprPtr> iterBody;
        for (auto& b : bodyExprs) iterBody.push_back(std::move(b));
        iterBody.push_back(std::move(loopCallExpr));
        auto iterBegin = make_begin(lst.span, std::move(iterBody));

        // Exit body: (begin expr...)  or void if no exit exprs
        SExprPtr exitBegin;
        if (exitExprs.empty()) {
            exitBegin = make_begin(lst.span, {});
        } else {
            exitBegin = make_begin(lst.span, std::move(exitExprs));
        }

        // (if test exit-begin iter-begin)
        auto ifExpr = make_if(lst.span, std::move(testExpr), std::move(exitBegin), std::move(iterBegin));

        // Build lambda: (lambda (var...) if-expr)
        auto formalsNode = make_list({}, lst.span);
        formalsNode->as<List>()->elems = std::move(params);
        std::vector<SExprPtr> lamBody;
        lamBody.push_back(std::move(ifExpr));
        auto lamExpr = make_lambda(lst.span, std::move(formalsNode), std::move(lamBody));

        // Build letrec: (letrec ((loop lambda)) (loop init...))
        auto bindingPair = build_list(lst.span, make_symbol(loopName, lst.span), std::move(lamExpr));
        auto bindingList = build_list(lst.span, std::move(bindingPair));

        std::vector<SExprPtr> initCall;
        initCall.push_back(make_symbol(loopName, lst.span));
        for (auto& i : inits) initCall.push_back(std::move(i));
        auto initCallExpr = make_list(std::move(initCall), lst.span);

        auto letrecForm = build_list(lst.span,
            make_symbol("letrec", lst.span),
            std::move(bindingList),
            std::move(initCallExpr));

        return handle_letrec(*letrecForm->as<List>());
    }

    ExpanderResult<SExprPtr> Expander::handle_module_list(const List& lst) {
        // (module name form...)
        if (lst.elems.size() < 2 || !lst.elems[1] || !lst.elems[1]->is<Symbol>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "module expects (module name forms...)"});


        std::vector<SExprPtr> xs; xs.reserve(lst.elems.size());
        xs.push_back(make_symbol("module", lst.span));
        xs.push_back(deep_clone(lst.elems[1]));
        // Expand body forms; splice top-level (begin ...) per R7RS
        for (size_t i=2;i<lst.elems.size();++i) {
            auto r = expand_form(lst.elems[i]);
            if (!r) return std::unexpected(r.error());
            if (auto* bl = r->get()->as<List>();
                bl && !bl->elems.empty() && is_symbol_named(bl->elems[0], "begin")) {
                for (size_t j = 1; j < bl->elems.size(); ++j)
                    xs.push_back(deep_clone(bl->elems[j]));
            } else {
                xs.push_back(std::move(*r));
            }
        }
        return make_list(std::move(xs), lst.span);
    }

    // ---------- Module directives ----------
    // For now, export/import are validated and passed through (no expansion of operands).
    ExpanderResult<SExprPtr> Expander::handle_export(const List& lst) {
        if (lst.dotted)
            return std::unexpected(invalid_syntax(lst.span, "export", "proper list required"));
        // Validate that all operands are symbols (exported names)
        for (size_t i = 1; i < lst.elems.size(); ++i) {
            if (!lst.elems[i] || !lst.elems[i]->is<Symbol>())
                return std::unexpected(invalid_syntax(lst.span, "export", "expected symbol name(s)"));
        }
        std::vector<SExprPtr> xs; xs.reserve(lst.elems.size());
        xs.push_back(make_symbol("export", lst.span));
        for (size_t i = 1; i < lst.elems.size(); ++i) xs.push_back(deep_clone(lst.elems[i]));
        return make_list(std::move(xs), lst.span);
    }

    ExpanderResult<SExprPtr> Expander::handle_import(const List& lst) {
        if (lst.dotted)
            return std::unexpected(invalid_syntax(lst.span, "import", "proper list required"));
        // Minimal validation: accept symbols as module names/specifiers; keep structure otherwise
        for (size_t i = 1; i < lst.elems.size(); ++i) {
            // Allow either symbol or list specifiers such as (only m x y) in future
            if (!lst.elems[i])
                return std::unexpected(invalid_syntax(lst.span, "import", "unexpected null operand"));
        }
        std::vector<SExprPtr> xs; xs.reserve(lst.elems.size());
        xs.push_back(make_symbol("import", lst.span));
        for (size_t i = 1; i < lst.elems.size(); ++i) xs.push_back(deep_clone(lst.elems[i]));
        return make_list(std::move(xs), lst.span);
    }

    // ---------- NEW: def and step ----------
    // (def x e) → (define x e)
    // (def (f args...) body...) → (define f (lambda (args...) body...))
    ExpanderResult<SExprPtr> Expander::handle_def(const List& lst) {
        if (lst.elems.size() < 2)
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "def expects at least a name"});

        // Optional explicit reserved-name check for the simple identifier case
        if (lst.elems[1] && lst.elems[1]->is<Symbol>()) {
            const auto* s = lst.elems[1]->as<Symbol>();
            if (is_reserved(s->name))
                return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, s->span, "cannot define reserved keyword: "+s->name});
        }

        // Rebuild as a (define ...) form and reuse handle_define
        std::vector<SExprPtr> xs;
        xs.push_back(make_symbol("define", lst.span));
        for (size_t i = 1; i < lst.elems.size(); ++i) {
            xs.push_back(deep_clone(lst.elems[i]));
        }
        auto asDefine = make_list(std::move(xs), lst.span);
        return handle_define(*asDefine->as<List>());
    }

    // (defun name (args...) body...) → (define (name args...) body...) and reuse define expansion
    ExpanderResult<SExprPtr> Expander::handle_defun(const List& lst) {
        if (lst.elems.size() < 3)
            return std::unexpected(invalid_syntax(lst.span, "defun", "expected (defun name (args...) body...)"));
        if (!lst.elems[1] || !lst.elems[1]->is<Symbol>())
            return std::unexpected(invalid_syntax(lst.span, "defun", "name must be a symbol"));

        const auto* nameSym = lst.elems[1]->as<Symbol>();
        if (is_reserved(nameSym->name))
            return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, nameSym->span, "cannot define reserved keyword: "+nameSym->name});

        // Formals can be a list or a single symbol (rest-only)
        if (!lst.elems[2])
            return std::unexpected(invalid_syntax(lst.span, "defun", "missing formals"));

        std::vector<SExprPtr> headWithName;
        headWithName.push_back(make_symbol(nameSym->name, nameSym->span));

        SExprPtr defineHead;
        if (lst.elems[2]->is<List>()) {
            const auto& formals = *lst.elems[2]->as<List>();
            // Build (name args...) possibly dotted
            std::vector<SExprPtr> sigElems; sigElems.reserve(formals.elems.size() + 1);
            sigElems.push_back(make_symbol(nameSym->name, nameSym->span));
            for (const auto& e : formals.elems) sigElems.push_back(deep_clone(e));
            if (formals.dotted && formals.tail) {
                defineHead = make_dotted_list(std::move(sigElems), deep_clone(formals.tail), formals.span);
            } else {
                defineHead = make_list(std::move(sigElems), formals.span);
            }
        } else if (lst.elems[2]->is<Symbol>()) {
            // (defun f rest body...) -> (define (f . rest) ...)
            std::vector<SExprPtr> sigElems;
            sigElems.push_back(make_symbol(nameSym->name, nameSym->span));
            defineHead = make_dotted_list(std::move(sigElems), deep_clone(lst.elems[2]), lst.elems[2]->span());
        } else {
            return std::unexpected(invalid_syntax(lst.span, "defun", "formals must be a list or symbol"));
        }

        std::vector<SExprPtr> defElems;
        defElems.push_back(make_symbol("define", lst.span));
        defElems.push_back(std::move(defineHead));
        for (size_t i = 3; i < lst.elems.size(); ++i) defElems.push_back(deep_clone(lst.elems[i]));
        auto asDefine = make_list(std::move(defElems), lst.span);
        return handle_define(*asDefine->as<List>());
    }

    // ---------- define-record-type (SRFI-9 style) ----------
    ExpanderResult<SExprPtr> Expander::handle_define_record_type(const List& lst) {
        // (define-record-type <type-name>
        //   (<constructor> <field-name> ...)
        //   <predicate>
        //   (<field-name> <accessor>)                ;; read-only
        //   (<field-name> <accessor> <mutator>))      ;; mutable
        const auto sp = lst.span;

        // --- Arity: at least 4 elements (keyword type-name ctor-spec predicate) ---
        if (lst.elems.size() < 4)
            return std::unexpected(ExpandError{ExpandError::Kind::ArityError, sp,
                "define-record-type: expected at least type-name, constructor, and predicate"});

        // --- Parse <type-name> ---
        const auto* typeNameSym = lst.elems[1] && lst.elems[1]->is<Symbol>()
            ? lst.elems[1]->as<Symbol>() : nullptr;
        if (!typeNameSym)
            return std::unexpected(syntax_error(lst.elems[1] ? lst.elems[1]->span() : sp,
                "define-record-type: type name must be a symbol"));
        const std::string& typeName = typeNameSym->name;
        if (is_reserved(typeName))
            return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, typeNameSym->span,
                "define-record-type: reserved keyword as type name: " + typeName});

        // --- Parse (<constructor> <field-name> ...) ---
        if (!lst.elems[2] || !lst.elems[2]->is<List>())
            return std::unexpected(syntax_error(lst.elems[2] ? lst.elems[2]->span() : sp,
                "define-record-type: constructor spec must be a list"));
        const auto& ctorSpec = *lst.elems[2]->as<List>();
        if (ctorSpec.elems.empty() || !ctorSpec.elems[0] || !ctorSpec.elems[0]->is<Symbol>())
            return std::unexpected(syntax_error(ctorSpec.span,
                "define-record-type: constructor name must be a symbol"));
        const std::string& ctorName = ctorSpec.elems[0]->as<Symbol>()->name;
        if (is_reserved(ctorName))
            return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, ctorSpec.elems[0]->span(),
                "define-record-type: reserved keyword as constructor name: " + ctorName});

        // Collect constructor field names and check for duplicates
        std::vector<std::string> ctorFields;
        std::unordered_set<std::string> fieldSeen;
        for (size_t i = 1; i < ctorSpec.elems.size(); ++i) {
            if (!ctorSpec.elems[i] || !ctorSpec.elems[i]->is<Symbol>())
                return std::unexpected(syntax_error(ctorSpec.elems[i] ? ctorSpec.elems[i]->span() : ctorSpec.span,
                    "define-record-type: constructor field name must be a symbol"));
            const auto& fname = ctorSpec.elems[i]->as<Symbol>()->name;
            if (is_reserved(fname))
                return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, ctorSpec.elems[i]->span(),
                    "define-record-type: reserved keyword as field name: " + fname});
            if (!fieldSeen.insert(fname).second)
                return std::unexpected(ExpandError{ExpandError::Kind::DuplicateIdentifier, ctorSpec.elems[i]->span(),
                    "define-record-type: duplicate field name in constructor: " + fname});
            ctorFields.push_back(fname);
        }

        // --- Parse <predicate> ---
        const auto* predSym = lst.elems[3] && lst.elems[3]->is<Symbol>()
            ? lst.elems[3]->as<Symbol>() : nullptr;
        if (!predSym)
            return std::unexpected(syntax_error(lst.elems[3] ? lst.elems[3]->span() : sp,
                "define-record-type: predicate must be a symbol"));
        const std::string& predName = predSym->name;
        if (is_reserved(predName))
            return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, predSym->span,
                "define-record-type: reserved keyword as predicate name: " + predName});

        // --- Parse field specs ---
        // Build a map: field-name -> index in ctorFields (1-based, slot 0 is tag)
        std::unordered_map<std::string, size_t> fieldIndex;
        for (size_t i = 0; i < ctorFields.size(); ++i)
            fieldIndex[ctorFields[i]] = i + 1; // slot 0 = type tag

        struct FieldSpec {
            std::string name;
            std::string accessor;
            std::string mutator; // empty if read-only
            size_t slot;
        };
        std::vector<FieldSpec> fieldSpecs;
        std::unordered_set<std::string> specifiedFields;

        for (size_t i = 4; i < lst.elems.size(); ++i) {
            if (!lst.elems[i] || !lst.elems[i]->is<List>())
                return std::unexpected(syntax_error(lst.elems[i] ? lst.elems[i]->span() : sp,
                    "define-record-type: field spec must be a list"));
            const auto& fspec = *lst.elems[i]->as<List>();
            if (fspec.elems.size() < 2 || fspec.elems.size() > 3)
                return std::unexpected(syntax_error(fspec.span,
                    "define-record-type: field spec must be (field accessor) or (field accessor mutator)"));

            // field name
            if (!fspec.elems[0] || !fspec.elems[0]->is<Symbol>())
                return std::unexpected(syntax_error(fspec.elems[0] ? fspec.elems[0]->span() : fspec.span,
                    "define-record-type: field name in spec must be a symbol"));
            const auto& fname = fspec.elems[0]->as<Symbol>()->name;
            auto idxIt = fieldIndex.find(fname);
            if (idxIt == fieldIndex.end())
                return std::unexpected(syntax_error(fspec.elems[0]->span(),
                    "define-record-type: field spec references unknown field: " + fname));
            if (!specifiedFields.insert(fname).second)
                return std::unexpected(ExpandError{ExpandError::Kind::DuplicateIdentifier, fspec.elems[0]->span(),
                    "define-record-type: duplicate field spec for: " + fname});

            // accessor
            if (!fspec.elems[1] || !fspec.elems[1]->is<Symbol>())
                return std::unexpected(syntax_error(fspec.elems[1] ? fspec.elems[1]->span() : fspec.span,
                    "define-record-type: accessor must be a symbol"));
            const auto& accName = fspec.elems[1]->as<Symbol>()->name;
            if (is_reserved(accName))
                return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, fspec.elems[1]->span(),
                    "define-record-type: reserved keyword as accessor: " + accName});

            // optional mutator
            std::string mutName;
            if (fspec.elems.size() == 3) {
                if (!fspec.elems[2] || !fspec.elems[2]->is<Symbol>())
                    return std::unexpected(syntax_error(fspec.elems[2] ? fspec.elems[2]->span() : fspec.span,
                        "define-record-type: mutator must be a symbol"));
                mutName = fspec.elems[2]->as<Symbol>()->name;
                if (is_reserved(mutName))
                    return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, fspec.elems[2]->span(),
                        "define-record-type: reserved keyword as mutator: " + mutName});
            }

            fieldSpecs.push_back(FieldSpec{fname, accName, mutName, idxIt->second});
        }

        // --- Generate type tag via gensym for generative semantics ---
        std::string typeTag = gensym(typeName);

        // Total vector size: 1 (tag) + number of fields
        size_t vecSize = 1 + ctorFields.size();

        // --- Build desugared (begin ...) ---
        std::vector<SExprPtr> defs;

        // 1) Constructor:
        //   (define (<ctor> f1 f2 ...)
        //     (let ((r (make-vector <N> '())))
        //       (vector-set! r 0 '<tag>)
        //       (vector-set! r 1 f1)
        //       ...
        //       r))
        {
            // Number literal for vector size
            auto makeNum = [&](int64_t n) -> SExprPtr {
                auto p = std::make_unique<SExpr>();
                parser::Number num; num.span = sp; num.value = n;
                p->value = std::move(num);
                return p;
            };

            // let-body: vector-set! calls + return r
            std::string rVar = gensym("r");
            std::vector<SExprPtr> letBody;

            // (vector-set! r 0 '<tag>)
            letBody.push_back(make_form(sp, "vector-set!",
                make_symbol(rVar, sp),
                makeNum(0),
                make_quote(make_symbol(typeTag, sp), sp)));

            // (vector-set! r <i> fi) for each field
            for (size_t i = 0; i < ctorFields.size(); ++i) {
                letBody.push_back(make_form(sp, "vector-set!",
                    make_symbol(rVar, sp),
                    makeNum(static_cast<int64_t>(i + 1)),
                    make_symbol(ctorFields[i], sp)));
            }

            // Return r
            letBody.push_back(make_symbol(rVar, sp));

            // (let ((r (make-vector <N> '()))) ...body)
            auto makeVecCall = make_form(sp, "make-vector",
                makeNum(static_cast<int64_t>(vecSize)),
                make_quote(make_nil(sp), sp));

            std::vector<std::pair<SExprPtr, SExprPtr>> bindings;
            bindings.emplace_back(make_symbol(rVar, sp), std::move(makeVecCall));
            auto letExpr = make_let(sp, std::move(bindings), std::move(letBody));

            // (define (<ctor> f1 f2 ...) let-expr)
            std::vector<SExprPtr> sigElems;
            sigElems.push_back(make_symbol(ctorName, sp));
            for (const auto& f : ctorFields) sigElems.push_back(make_symbol(f, sp));
            auto sig = make_list(std::move(sigElems), sp);

            defs.push_back(make_form(sp, "define", std::move(sig), std::move(letExpr)));
        }

        // 2) Predicate:
        //   (define (<pred> obj)
        //     (and (vector? obj)
        //          (> (vector-length obj) 0)
        //          (eq? (vector-ref obj 0) '<tag>)))
        {
            std::string objVar = gensym("obj");

            auto makeNum = [&](int64_t n) -> SExprPtr {
                auto p = std::make_unique<SExpr>();
                parser::Number num; num.span = sp; num.value = n;
                p->value = std::move(num);
                return p;
            };

            auto testVec = make_form(sp, "vector?", make_symbol(objVar, sp));
            auto testLen = make_form(sp, ">",
                make_form(sp, "vector-length", make_symbol(objVar, sp)),
                makeNum(0));
            auto testTag = make_form(sp, "eq?",
                make_form(sp, "vector-ref", make_symbol(objVar, sp), makeNum(0)),
                make_quote(make_symbol(typeTag, sp), sp));

            auto body = make_form(sp, "and",
                std::move(testVec), std::move(testLen), std::move(testTag));

            auto sig = build_list(sp, make_symbol(predName, sp), make_symbol(objVar, sp));
            defs.push_back(make_form(sp, "define", std::move(sig), std::move(body)));
        }

        // 3) Accessors:
        //   (define (<accessor> obj) (vector-ref obj <index>))
        for (const auto& fs : fieldSpecs) {
            std::string objVar = gensym("obj");
            auto makeNum = [&](int64_t n) -> SExprPtr {
                auto p = std::make_unique<SExpr>();
                parser::Number num; num.span = sp; num.value = n;
                p->value = std::move(num);
                return p;
            };

            auto body = make_form(sp, "vector-ref",
                make_symbol(objVar, sp), makeNum(static_cast<int64_t>(fs.slot)));
            auto sig = build_list(sp, make_symbol(fs.accessor, sp), make_symbol(objVar, sp));
            defs.push_back(make_form(sp, "define", std::move(sig), std::move(body)));
        }

        // 4) Mutators (only for mutable fields):
        //   (define (<mutator> obj val) (vector-set! obj <index> val))
        for (const auto& fs : fieldSpecs) {
            if (fs.mutator.empty()) continue;
            std::string objVar = gensym("obj");
            std::string valVar = gensym("val");
            auto makeNum = [&](int64_t n) -> SExprPtr {
                auto p = std::make_unique<SExpr>();
                parser::Number num; num.span = sp; num.value = n;
                p->value = std::move(num);
                return p;
            };

            auto body = make_form(sp, "vector-set!",
                make_symbol(objVar, sp), makeNum(static_cast<int64_t>(fs.slot)),
                make_symbol(valVar, sp));
            auto sig = build_list(sp, make_symbol(fs.mutator, sp),
                make_symbol(objVar, sp), make_symbol(valVar, sp));
            defs.push_back(make_form(sp, "define", std::move(sig), std::move(body)));
        }

        // Wrap in (begin ...) and re-expand
        auto beginForm = make_begin(sp, std::move(defs));
        return expand_form(beginForm);
    }

    // ---------- Internal defines → letrec (optional) ----------
    // If the lambda body starts with one or more define forms, rewrite to a single
    // (letrec ((name init) ...) body ...)
    // Returns true if a rewrite occurred, and places the letrec in body[0].
    ExpanderResult<bool> Expander::rewrite_internal_defines_to_letrec(std::vector<SExprPtr>& body) const {
        if (body.empty()) return false;

        // Collect leading defines
        struct Def { SExprPtr name; SExprPtr init; Span span; };
        std::vector<Def> defs;
        size_t idx = 0;
        for (; idx < body.size(); ++idx) {
            const auto& f = body[idx];
            if (!f || !f->is<List>()) break;
            const auto& l = *f->as<List>();
            if (l.elems.empty() || !l.elems[0] || !l.elems[0]->is<Symbol>() || l.elems[0]->as<Symbol>()->name != "define")
                break;
            if (l.elems.size() < 3) return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, l.span, "define expects name and value"});

            // (define id expr)
            if (l.elems[1] && l.elems[1]->is<Symbol>()) {
                const auto* s = l.elems[1]->as<Symbol>();
                if (is_reserved(s->name)) return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, s->span, "cannot define reserved keyword: "+s->name});
                defs.push_back(Def{ make_symbol(s->name, s->span), deep_clone(l.elems[2]), l.span });
                continue;
            }

            // (define (f args...) body...)
            if (!l.elems[1] || !l.elems[1]->is<List>())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, l.span, "bad define head"});
            const auto& sig = *l.elems[1]->as<List>();
            if (sig.elems.empty() || !sig.elems[0] || !sig.elems[0]->is<Symbol>())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, sig.span, "bad define head"});
            const auto* nameSym = sig.elems[0]->as<Symbol>();
            if (is_reserved(nameSym->name))
                return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, nameSym->span, "cannot define reserved keyword: "+nameSym->name});

            // Build lambda node (unexpanded; full expand happens later)
            SExprPtr formalsNode;
            if (sig.elems.size() == 1 && sig.dotted && sig.tail && sig.tail->is<Symbol>()) {
                formalsNode = deep_clone(sig.tail);
            } else {
                std::vector<SExprPtr> params;
                for (size_t i=1;i<sig.elems.size();++i) params.push_back(deep_clone(sig.elems[i]));
                formalsNode = sig.dotted && sig.tail ? make_dotted_list(std::move(params), deep_clone(sig.tail), sig.span)
                                                     : make_list(std::move(params), sig.span);
            }
            std::vector<SExprPtr> lam; lam.push_back(make_symbol("lambda", l.span));
            lam.push_back(std::move(formalsNode));
            for (size_t i=2;i<l.elems.size();++i) lam.push_back(deep_clone(l.elems[i]));
            auto lamE = make_list(std::move(lam), l.span);

            defs.push_back(Def{ make_symbol(nameSym->name, nameSym->span), std::move(lamE), l.span });
        }

        if (defs.empty()) return false;

        // Build letrec form: (letrec ((name init) ...) remaining-body...)
        auto firstSpan = body[0]->span();
        auto bindingList = make_list({}, firstSpan);
        for (auto& d : defs) {
            auto pair = make_list({}, d.span);
            pair->as<List>()->elems.push_back(deep_clone(d.name));
            pair->as<List>()->elems.push_back(deep_clone(d.init));
            bindingList->as<List>()->elems.push_back(std::move(pair));
        }

        std::vector<SExprPtr> remaining;
        for (size_t i=idx; i<body.size(); ++i) remaining.push_back(deep_clone(body[i]));

        std::vector<SExprPtr> letrec;
        letrec.push_back(make_symbol("letrec", firstSpan));
        letrec.push_back(std::move(bindingList));
        for (auto& r : remaining) letrec.push_back(std::move(r));

        body.clear();
        body.push_back(make_list(std::move(letrec), firstSpan));
        return true;
    }

    // ========================================================================
    // define-syntax / syntax-rules
    // ========================================================================

    // ---------- handle_define_syntax ----------
    // (define-syntax <name> (syntax-rules (<literal> ...) <clause> ...))
    // Each clause is (<pattern> <template>).
    ExpanderResult<SExprPtr> Expander::handle_define_syntax(const List& lst) {
        const auto sp = lst.span;
        if (lst.elems.size() != 3)
            return std::unexpected(syntax_error(sp, "define-syntax expects (define-syntax name (syntax-rules ...))"));

        // Parse name
        if (!lst.elems[1] || !lst.elems[1]->is<Symbol>())
            return std::unexpected(syntax_error(sp, "define-syntax: name must be a symbol"));
        const auto& macroName = lst.elems[1]->as<Symbol>()->name;

        // Parse (syntax-rules (<literal> ...) <clause> ...)
        if (!lst.elems[2] || !lst.elems[2]->is<List>())
            return std::unexpected(syntax_error(sp, "define-syntax: expected (syntax-rules ...)"));
        const auto& srForm = *lst.elems[2]->as<List>();
        if (srForm.elems.empty() || !is_symbol_named(srForm.elems[0], "syntax-rules"))
            return std::unexpected(syntax_error(sp, "define-syntax: transformer must be (syntax-rules ...)"));
        if (srForm.elems.size() < 2)
            return std::unexpected(syntax_error(srForm.span, "syntax-rules: expected literal list"));

        // Parse literal list
        if (!srForm.elems[1] || !srForm.elems[1]->is<List>())
            return std::unexpected(syntax_error(srForm.span, "syntax-rules: literals must be a list of symbols"));
        const auto& litList = *srForm.elems[1]->as<List>();
        std::unordered_set<std::string> literalSet;
        SyntaxRulesTransformer transformer;
        for (const auto& elem : litList.elems) {
            if (!elem || !elem->is<Symbol>())
                return std::unexpected(syntax_error(litList.span, "syntax-rules: each literal must be a symbol"));
            const auto& lname = elem->as<Symbol>()->name;
            literalSet.insert(lname);
            transformer.literals.push_back(lname);
        }

        // Parse clauses
        for (std::size_t i = 2; i < srForm.elems.size(); ++i) {
            if (!srForm.elems[i] || !srForm.elems[i]->is<List>())
                return std::unexpected(syntax_error(srForm.span, "syntax-rules: each clause must be a list"));
            const auto& clause = *srForm.elems[i]->as<List>();
            if (clause.elems.size() != 2)
                return std::unexpected(syntax_error(clause.span, "syntax-rules: clause must be (pattern template)"));

            // The pattern must be a list; element 0 is the macro-name placeholder (ignored)
            if (!clause.elems[0] || !clause.elems[0]->is<List>())
                return std::unexpected(syntax_error(clause.span, "syntax-rules: pattern must be a list"));
            const auto& patList = *clause.elems[0]->as<List>();
            if (patList.elems.empty())
                return std::unexpected(syntax_error(patList.span, "syntax-rules: pattern must not be empty"));

            // Build a PatList from elements 1..N (skip element 0 = macro name placeholder)
            std::unordered_set<std::string> boundVars;
            auto patListNode = std::make_unique<SyntaxPattern>();
            PatList pl;
            bool sawEllipsis = false;
            for (std::size_t j = 1; j < patList.elems.size(); ++j) {
                // Check if this element is an ellipsis
                if (patList.elems[j] && patList.elems[j]->is<Symbol>() &&
                    patList.elems[j]->as<Symbol>()->name == "...") {
                    if (sawEllipsis)
                        return std::unexpected(syntax_error(patList.span, "syntax-rules: multiple ellipses in pattern"));
                    if (pl.elems.empty())
                        return std::unexpected(syntax_error(patList.span, "syntax-rules: ellipsis without preceding pattern"));
                    sawEllipsis = true;
                    pl.ellipsis_pat = std::move(pl.elems.back());
                    pl.elems.pop_back();
                    pl.ellipsis_index = pl.elems.size();
                    continue;
                }
                auto sub = parse_syntax_pattern(patList.elems[j], literalSet, boundVars);
                if (!sub) return std::unexpected(sub.error());
                pl.elems.push_back(std::move(*sub));
            }
            patListNode->data = std::move(pl);

            // Collect pattern variables for template parsing
            std::unordered_set<std::string> patVars;
            collect_pattern_vars(*patListNode, patVars);

            // Parse template
            auto tmpl = parse_syntax_template(clause.elems[1], patVars);
            if (!tmpl) return std::unexpected(tmpl.error());

            SyntaxClause sc;
            sc.pattern = std::move(patListNode);
            sc.tmpl = std::move(*tmpl);
            sc.span = clause.span;
            transformer.clauses.push_back(std::move(sc));
        }

        macro_env_[macroName] = std::move(transformer);

        // define-syntax produces no runtime output
        return make_begin(sp, {});
    }

    // ---------- parse_syntax_pattern ----------
    ExpanderResult<SyntaxPatternPtr> Expander::parse_syntax_pattern(
        const SExprPtr& node,
        const std::unordered_set<std::string>& literals,
        const std::unordered_set<std::string>& /*bound_vars*/) const
    {
        auto pat = std::make_unique<SyntaxPattern>();

        if (!node)
            return std::unexpected(syntax_error(Span{}, "syntax-rules: null element in pattern"));

        // Symbol cases
        if (node->is<Symbol>()) {
            const auto& name = node->as<Symbol>()->name;
            if (name == "_") {
                pat->data = PatUnderscore{};
            } else if (literals.contains(name)) {
                pat->data = PatLiteral{name};
            } else {
                pat->data = PatVar{name};
            }
            return pat;
        }

        // List case
        if (node->is<List>()) {
            const auto& lst = *node->as<List>();
            PatList pl;
            bool sawEllipsis = false;
            for (std::size_t i = 0; i < lst.elems.size(); ++i) {
                if (lst.elems[i] && lst.elems[i]->is<Symbol>() &&
                    lst.elems[i]->as<Symbol>()->name == "...") {
                    if (sawEllipsis)
                        return std::unexpected(syntax_error(lst.span, "syntax-rules: multiple ellipses in pattern"));
                    if (pl.elems.empty())
                        return std::unexpected(syntax_error(lst.span, "syntax-rules: ellipsis without preceding pattern"));
                    sawEllipsis = true;
                    pl.ellipsis_pat = std::move(pl.elems.back());
                    pl.elems.pop_back();
                    pl.ellipsis_index = pl.elems.size();
                    continue;
                }
                auto sub = parse_syntax_pattern(lst.elems[i], literals, {});
                if (!sub) return std::unexpected(sub.error());
                pl.elems.push_back(std::move(*sub));
            }
            pat->data = std::move(pl);
            return pat;
        }

        // Datum (number, bool, char, string)
        pat->data = PatDatum{deep_clone(node)};
        return pat;
    }

    // ---------- parse_syntax_template ----------
    ExpanderResult<SyntaxTemplatePtr> Expander::parse_syntax_template(
        const SExprPtr& node,
        const std::unordered_set<std::string>& pattern_vars) const
    {
        auto tmpl = std::make_unique<SyntaxTemplate>();

        if (!node)
            return std::unexpected(syntax_error(Span{}, "syntax-rules: null element in template"));

        // Symbol cases
        if (node->is<Symbol>()) {
            const auto& name = node->as<Symbol>()->name;
            if (pattern_vars.contains(name)) {
                tmpl->data = TmplVar{name};
            } else {
                tmpl->data = TmplSymbol{name};
            }
            return tmpl;
        }

        // List case
        if (node->is<List>()) {
            const auto& lst = *node->as<List>();
            TmplList tl;
            bool sawEllipsis = false;
            for (std::size_t i = 0; i < lst.elems.size(); ++i) {
                if (lst.elems[i] && lst.elems[i]->is<Symbol>() &&
                    lst.elems[i]->as<Symbol>()->name == "...") {
                    if (sawEllipsis)
                        return std::unexpected(syntax_error(lst.span, "syntax-rules: multiple ellipses in template"));
                    if (tl.elems.empty())
                        return std::unexpected(syntax_error(lst.span, "syntax-rules: ellipsis without preceding template"));
                    sawEllipsis = true;
                    tl.ellipsis_tmpl = std::move(tl.elems.back());
                    tl.elems.pop_back();
                    tl.ellipsis_index = tl.elems.size();
                    continue;
                }
                auto sub = parse_syntax_template(lst.elems[i], pattern_vars);
                if (!sub) return std::unexpected(sub.error());
                tl.elems.push_back(std::move(*sub));
            }
            tmpl->data = std::move(tl);
            return tmpl;
        }

        // Datum (number, bool, char, string, etc.)
        tmpl->data = TmplDatum{deep_clone(node)};
        return tmpl;
    }

    // ---------- collect_pattern_vars ----------
    void Expander::collect_pattern_vars(const SyntaxPattern& pat,
                                        std::unordered_set<std::string>& out) {
        std::visit([&](auto&& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, PatVar>) {
                out.insert(p.name);
            } else if constexpr (std::is_same_v<T, PatList>) {
                for (const auto& sub : p.elems) collect_pattern_vars(*sub, out);
                if (p.ellipsis_pat) collect_pattern_vars(**p.ellipsis_pat, out);
            }
            // PatUnderscore, PatLiteral, PatDatum: no variables
        }, pat.data);
    }

    // ---------- match_pattern ----------

    // Helper: compare two SExpr datums for structural equality (used by PatDatum)
    static bool datum_equal(const SExprPtr& a, const SExprPtr& b) {
        if (!a || !b) return false;
        if (a->is<parser::Bool>() && b->is<parser::Bool>())
            return a->as<parser::Bool>()->value == b->as<parser::Bool>()->value;
        if (a->is<parser::Number>() && b->is<parser::Number>())
            return a->as<parser::Number>()->value == b->as<parser::Number>()->value;
        if (a->is<parser::Char>() && b->is<parser::Char>())
            return a->as<parser::Char>()->value == b->as<parser::Char>()->value;
        if (a->is<parser::String>() && b->is<parser::String>())
            return a->as<parser::String>()->value == b->as<parser::String>()->value;
        return false;
    }

    // Returns true if input matches the pattern, populating env with bindings.
    bool Expander::match_pattern(const SyntaxPattern& pat,
                                 const SExprPtr& input,
                                 MatchEnv& env) {
        return std::visit([&](auto&& p) -> bool {
            using T = std::decay_t<decltype(p)>;

            if constexpr (std::is_same_v<T, PatVar>) {
                env[p.name] = MatchBinding{deep_clone(input), {}, false};
                return true;
            }
            else if constexpr (std::is_same_v<T, PatUnderscore>) {
                return true; // matches anything
            }
            else if constexpr (std::is_same_v<T, PatLiteral>) {
                return input && input->is<Symbol>() && input->as<Symbol>()->name == p.name;
            }
            else if constexpr (std::is_same_v<T, PatDatum>) {
                return datum_equal(p.datum, input);
            }
            else if constexpr (std::is_same_v<T, PatList>) {
                if (!input || !input->is<List>()) return false;
                const auto& lst = *input->as<List>();
                if (lst.dotted) return false; // no dotted list matching yet

                if (!p.ellipsis_pat) {
                    // No ellipsis: exact length match
                    if (lst.elems.size() != p.elems.size()) return false;
                    for (std::size_t i = 0; i < p.elems.size(); ++i) {
                        if (!match_pattern(*p.elems[i], lst.elems[i], env))
                            return false;
                    }
                    return true;
                }

                // With ellipsis: fixed_before + repeated + fixed_after
                std::size_t fixedBefore = p.ellipsis_index;
                std::size_t fixedAfter = p.elems.size() - p.ellipsis_index;
                std::size_t totalFixed = fixedBefore + fixedAfter;
                if (lst.elems.size() < totalFixed) return false;
                std::size_t repeatCount = lst.elems.size() - totalFixed;

                // Match fixed elements before ellipsis
                for (std::size_t i = 0; i < fixedBefore; ++i) {
                    if (!match_pattern(*p.elems[i], lst.elems[i], env))
                        return false;
                }

                // Collect pattern vars from the ellipsis sub-pattern
                std::unordered_set<std::string> ellipsisVars;
                collect_pattern_vars(**p.ellipsis_pat, ellipsisVars);

                // Initialize repeated bindings
                for (const auto& vname : ellipsisVars) {
                    env[vname] = MatchBinding{nullptr, {}, true};
                }

                // Match each repeated element
                for (std::size_t r = 0; r < repeatCount; ++r) {
                    MatchEnv subEnv;
                    if (!match_pattern(**p.ellipsis_pat, lst.elems[fixedBefore + r], subEnv))
                        return false;
                    for (auto& [k, v] : subEnv) {
                        if (ellipsisVars.contains(k)) {
                            env[k].repeated.push_back(std::move(v.single));
                        }
                    }
                }

                // Match fixed elements after ellipsis
                for (std::size_t i = 0; i < fixedAfter; ++i) {
                    if (!match_pattern(*p.elems[fixedBefore + i],
                                       lst.elems[fixedBefore + repeatCount + i], env))
                        return false;
                }

                return true;
            }
            else {
                return false;
            }
        }, pat.data);
    }

    // ---------- instantiate_template ----------
    ExpanderResult<SExprPtr> Expander::instantiate_template(
        const SyntaxTemplate& tmpl,
        const MatchEnv& env,
        std::unordered_map<std::string, std::string>& renames,
        Span ctx) const
    {
        return std::visit([&](auto&& t) -> ExpanderResult<SExprPtr> {
            using T = std::decay_t<decltype(t)>;

            if constexpr (std::is_same_v<T, TmplVar>) {
                auto it = env.find(t.name);
                if (it == env.end())
                    return std::unexpected(syntax_error(ctx, "syntax-rules: unbound pattern variable in template: " + t.name));
                if (it->second.is_ellipsis)
                    return std::unexpected(syntax_error(ctx, "syntax-rules: ellipsis variable used outside ellipsis context: " + t.name));
                return deep_clone(it->second.single);
            }
            else if constexpr (std::is_same_v<T, TmplDatum>) {
                return deep_clone(t.datum);
            }
            else if constexpr (std::is_same_v<T, TmplSymbol>) {
                // Hygienic renaming for introduced identifiers:
                // If the symbol is not a keyword/special, rename it so it can't
                // capture or be captured by user-level bindings.
                // We skip renaming for well-known forms that the expander/SA must see literally.
                static const std::unordered_set<std::string> passthrough = {
                    "if","begin","lambda","define","set!","quote","let","let*",
                    "letrec","letrec*","cond","case","and","or","when","unless",
                    "do","quasiquote","unquote","unquote-splicing",
                    "module","import","export","define-syntax","define-record-type",
                    "cons","car","cdr","list","append","apply","not",
                    "eq?","eqv?","equal?","null?","pair?","number?","boolean?",
                    "string?","char?","symbol?","procedure?","integer?","vector?",
                    "zero?","positive?","negative?","abs","min","max","modulo","remainder",
                    "+","-","*","/","=","<",">","<=",">=",
                    "string-length","string-append","number->string","string->number",
                    "vector","vector-length","vector-ref","vector-set!","make-vector",
                    "map","for-each","length","reverse","list-ref","list-tail",
                    "set-car!","set-cdr!","error",
                    "call/cc","call-with-current-continuation","dynamic-wind",
                    "values","call-with-values",
                    "display","write","newline",
                    "#t","#f",
                    "def","defun","progn",
                };
                if (passthrough.contains(t.name) || macro_env_.contains(t.name)) {
                    return make_symbol(t.name, ctx);
                }
                // Apply hygiene: rename introduced identifiers
                auto rit = renames.find(t.name);
                if (rit == renames.end()) {
                    auto fresh = gensym(t.name);
                    renames[t.name] = fresh;
                    rit = renames.find(t.name);
                }
                return make_symbol(rit->second, ctx);
            }
            else if constexpr (std::is_same_v<T, TmplList>) {
                std::vector<SExprPtr> result;

                if (!t.ellipsis_tmpl) {
                    // No ellipsis: instantiate each element
                    result.reserve(t.elems.size());
                    for (const auto& sub : t.elems) {
                        auto r = instantiate_template(*sub, env, renames, ctx);
                        if (!r) return r;
                        result.push_back(std::move(*r));
                    }
                    return make_list(std::move(result), ctx);
                }

                // With ellipsis: fixed_before + repeated + fixed_after
                std::size_t fixedBefore = t.ellipsis_index;
                std::size_t fixedAfter = t.elems.size() - t.ellipsis_index;

                // Instantiate fixed elements before ellipsis
                for (std::size_t i = 0; i < fixedBefore; ++i) {
                    auto r = instantiate_template(*t.elems[i], env, renames, ctx);
                    if (!r) return r;
                    result.push_back(std::move(*r));
                }

                // Find the repeat count from any ellipsis-bound variable in the sub-template
                std::size_t repeatCount = 0;
                bool foundRepeat = false;
                for (const auto& [k, v] : env) {
                    if (v.is_ellipsis) {
                        repeatCount = v.repeated.size();
                        foundRepeat = true;
                        break;
                    }
                }

                // Instantiate the ellipsis sub-template once per repetition
                if (foundRepeat) {
                    for (std::size_t r = 0; r < repeatCount; ++r) {
                        // Build a sub-environment: non-ellipsis vars cloned, ellipsis vars resolved to iteration r
                        MatchEnv subEnv;
                        for (const auto& [k, v] : env) {
                            if (v.is_ellipsis) {
                                if (r < v.repeated.size()) {
                                    subEnv.emplace(k, MatchBinding{deep_clone(v.repeated[r]), {}, false});
                                } else {
                                    subEnv.emplace(k, MatchBinding{make_nil(ctx), {}, false});
                                }
                            } else {
                                subEnv.emplace(k, MatchBinding{deep_clone(v.single), {}, false});
                            }
                        }
                        auto inst = instantiate_template(**t.ellipsis_tmpl, subEnv, renames, ctx);
                        if (!inst) return inst;
                        result.push_back(std::move(*inst));
                    }
                }

                // Instantiate fixed elements after ellipsis
                for (std::size_t i = fixedBefore; i < fixedBefore + fixedAfter; ++i) {
                    auto r = instantiate_template(*t.elems[i], env, renames, ctx);
                    if (!r) return r;
                    result.push_back(std::move(*r));
                }

                return make_list(std::move(result), ctx);
            }
            else {
                return std::unexpected(syntax_error(ctx, "syntax-rules: unknown template kind"));
            }
        }, tmpl.data);
    }

    // ---------- try_expand_macro ----------
    ExpanderResult<SExprPtr> Expander::try_expand_macro(const std::string& name,
                                                         const List& lst) {
        auto it = macro_env_.find(name);
        if (it == macro_env_.end())
            return std::unexpected(syntax_error(lst.span, "undefined macro: " + name));

        const auto& transformer = it->second;

        // Build the input list excluding the macro name (elements 1..N)
        // The pattern was parsed with element 0 stripped, so we match against
        // the whole form but with element 0 skipped in the pattern.
        // Actually, our patterns already skip element 0. We need to match
        // the tail elements against the PatList.

        for (const auto& clause : transformer.clauses) {
            MatchEnv env;
            // The clause.pattern is a PatList of the tail (elements after macro name).
            // Build a temporary list of just the tail elements to match against.
            const auto* patList = std::get_if<PatList>(&clause.pattern->data);
            if (!patList) continue;

            // Match the tail elements of the input against the pattern
            // We need to match element-by-element against the pattern list
            std::vector<SExprPtr> tailElems;
            for (std::size_t i = 1; i < lst.elems.size(); ++i) {
                tailElems.push_back(deep_clone(lst.elems[i]));
            }
            auto tailList = make_list(std::move(tailElems), lst.span);

            if (match_pattern(*clause.pattern, tailList, env)) {
                std::unordered_map<std::string, std::string> renames;
                auto expanded = instantiate_template(*clause.tmpl, env, renames, lst.span);
                if (!expanded) return expanded;
                // Re-expand the output (macros can produce derived forms or other macro calls)
                return expand_form(*expanded);
            }
        }

        return std::unexpected(syntax_error(lst.span, "no matching clause for macro: " + name));
    }

}
