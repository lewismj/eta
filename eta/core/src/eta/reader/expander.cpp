#include "expander.h"

#include <cassert>
#include <cstdint>
#include <sstream>
#include <unordered_map>
#include <functional> // for std::function in cond builder (legacy)
#include <unordered_set>
#include <atomic>
#include <random>

namespace eta::reader::expander {

    // ---------- Reserved keywords ----------
    static const std::unordered_set<std::string>& reserved_keywords() {
        static const std::unordered_set<std::string> K = {
            // Core/derived
            "quote","if","begin","lambda","define","set!",
            "let","let*","letrec","cond","and","or",
            "quasiquote","unquote","unquote-splicing",
            // Modules/macros
            "module","import","export","define-syntax","syntax-rules",
            // Advanced control / multiple values (future)
            "call-with-current-continuation","call/cc","dynamic-wind",
            "values","call-with-values",
            // Convenience (existing and new)
            "case","do","when","unless",
            "def","defun","progn","step"
        };
        return K;
    }

    bool Expander::is_reserved(std::string_view name) { return reserved_keywords().count(std::string(name)) != 0; }

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

    SExprPtr Expander::deep_clone(const SExpr& n) {
        auto p = std::make_unique<SExpr>();

        if (auto a = n.as<parser::Nil>()) { parser::Nil c = *a; p->value = c; return p; }
        if (auto a = n.as<parser::Bool>()) { parser::Bool c = *a; p->value = c; return p; }
        if (auto a = n.as<parser::Char>()) { parser::Char c = *a; p->value = c; return p; }
        if (auto a = n.as<parser::String>()) { p->value = *a; return p; }
        if (auto a = n.as<parser::Symbol>()) { p->value = *a; return p; }
        if (auto a = n.as<parser::Number>()) { p->value = *a; return p; }
        if (auto a = n.as<parser::List>()) {
            parser::List l; l.span = a->span; l.dotted = a->dotted;
            l.elems.reserve(a->elems.size());
            for (const auto& e : a->elems) l.elems.push_back(deep_clone(e));
            if (a->dotted && a->tail) l.tail = deep_clone(a->tail);
            p->value = std::move(l);
            return p;
        }
        if (auto a = n.as<parser::Vector>()) {
            parser::Vector v2; v2.span = a->span; v2.elems.reserve(a->elems.size());
            for (const auto& e : a->elems) v2.elems.push_back(deep_clone(e));
            p->value = std::move(v2);
            return p;
        }
        if (auto a = n.as<parser::ByteVector>()) { p->value = *a; return p; }
        if (auto a = n.as<parser::ReaderForm>()) {
            parser::ReaderForm rf; rf.span = a->span; rf.kind = a->kind; rf.expr = deep_clone(a->expr);
            p->value = std::move(rf);
            return p;
        }
        if (auto a = n.as<parser::ModuleForm>()) {
            parser::ModuleForm m; m.span = a->span; m.name = a->name; m.exports = a->exports; m.body.reserve(a->body.size());
            for (const auto& e : a->body) m.body.push_back(deep_clone(e));
            p->value = std::move(m);
            return p;
        }
        assert(false && "Unknown SExpr variant in deep_clone");
        return nullptr;
    }

    bool Expander::is_symbol_named(const SExprPtr& p, std::string_view name) {
        if (!p) return false;
        if (auto s = p->as<Symbol>()) return s->name == name;
        return false;
    }

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
                case parser::QuoteKind::Quote:
                    return deep_clone(in);
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
            // Expand each body form
            for (const auto& form : mf->body) {
                auto exp = expand_form(form);
                if (!exp) return std::unexpected(exp.error());
                elems.push_back(std::move(*exp));
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
            {"module", &Expander::handle_module_list},
            {"export", &Expander::handle_export},
            {"import", &Expander::handle_import},
            // convenience
            {"def", &Expander::handle_def},
            {"step", &Expander::handle_begin},
            {"defun", &Expander::handle_defun},
            {"progn", &Expander::handle_begin},
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
        std::vector<SExprPtr> xs; xs.reserve(lst.elems.size());
        xs.push_back(deep_clone(lst.elems[0]));
        for (size_t i=1;i<lst.elems.size();++i) {
            auto r = expand_form(lst.elems[i]); if (!r) return std::unexpected(r.error()); xs.push_back(std::move(*r));
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
            std::vector<SExprPtr> v;
            v.push_back(deep_clone(lst.elems[0]));
            v.push_back(deep_clone(lst.elems[1]));
            v.push_back(std::move(*rhs));
            return make_list(std::move(v), lst.span);
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
        std::vector<SExprPtr> lam; lam.push_back(make_symbol("lambda", lst.span));
        lam.push_back(std::move(formalsNode));
        for (size_t i=2;i<lst.elems.size();++i) lam.push_back(deep_clone(lst.elems[i]));
        auto lamE = make_list(std::move(lam), lst.span);

        // Normalize lambda (validates formals and expands body)
        auto lamX = handle_lambda(*lamE->as<List>());
        if (!lamX) return std::unexpected(lamX.error());

        std::vector<SExprPtr> v;
        v.push_back(make_symbol("define", lst.span));
        v.push_back(make_symbol(nameSym->name, nameSym->span));
        v.push_back(std::move(*lamX));
        return make_list(std::move(v), lst.span);
    }

    ExpanderResult<SExprPtr> Expander::handle_set_bang(const List& lst) {
        if (lst.elems.size() != 3 || !lst.elems[1] || !lst.elems[1]->is<Symbol>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "set! expects (set! id expr)"});
        auto rhs = expand_form(lst.elems[2]); if (!rhs) return std::unexpected(rhs.error());
        std::vector<SExprPtr> v;
        v.push_back(deep_clone(lst.elems[0]));
        v.push_back(deep_clone(lst.elems[1]));
        v.push_back(std::move(*rhs));
        return make_list(std::move(v), lst.span);
    }

    ExpanderResult<Formals> Expander::parse_formals(const SExprPtr& node) const {
        Formals f; f.span = node->span();
        std::unordered_set<std::string> seen;
        auto dup = [&](const std::string& n, Span sp) -> ExpanderResult<Formals> {
            return std::unexpected(ExpandError{ExpandError::Kind::DuplicateIdentifier, sp, "duplicate parameter: "+n});
        };

        if (node->is<Symbol>()) {
            const auto* s = node->as<Symbol>();
            if (is_reserved(s->name)) return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, s->span, "reserved keyword as rest parameter: "+s->name});
            f.rest = s->name; return f;
        }
        if (!node->is<List>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, node->span(), "lambda formals must be a symbol or list"});

        const auto& lst = *node->as<List>();
        if (!lst.dotted) {
            for (size_t i=0; i<lst.elems.size(); ++i) {
                const auto& p = lst.elems[i];
                if (!p || !p->is<Symbol>()) {
                    return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, p ? p->span() : lst.span, "parameter at index "+std::to_string(i)+" must be a symbol"});
                }
                const auto* s = p->as<Symbol>();
                if (is_reserved(s->name)) return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, s->span, "reserved keyword as parameter: "+s->name});
                if (!seen.insert(s->name).second) return dup(s->name, s->span);
                f.fixed.push_back(s->name);
            }
            return f;
        }

        // Dotted list: head elems are fixed, tail must be a symbol
        for (size_t i=0; i<lst.elems.size(); ++i) {
            const auto& p = lst.elems[i];
            if (!p || !p->is<Symbol>()) return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, p ? p->span() : lst.span, "parameter at index "+std::to_string(i)+" must be a symbol"});
            const auto* s = p->as<Symbol>();
            if (is_reserved(s->name)) return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, s->span, "reserved keyword as parameter: "+s->name});
            if (!seen.insert(s->name).second) return dup(s->name, s->span);
            f.fixed.push_back(s->name);
        }
        if (!lst.tail || !lst.tail->is<Symbol>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "dotted formals: rest must be a symbol"});
        const auto* rest = lst.tail->as<Symbol>();
        if (is_reserved(rest->name)) return std::unexpected(ExpandError{ExpandError::Kind::ReservedKeyword, rest->span, "reserved keyword as parameter: "+rest->name});
        if (!seen.insert(rest->name).second) return dup(rest->name, rest->span);
        f.rest = rest->name;
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
        for (const auto& p : pair_list.elems) {
            if (!p || !p->is<List>())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidLetBindings, pair_list.span, "let binding must be (id expr)"});
            const auto& pr = *p->as<List>();
            if (pr.dotted || pr.elems.size() != 2 || !pr.elems[0] || !pr.elems[0]->is<Symbol>())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidLetBindings, pr.span, "let binding must be (id expr)"});
            const auto* id = pr.elems[0]->as<Symbol>();
            if (is_reserved(id->name))
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidLetBindings, pr.elems[0]->span(), "cannot bind reserved keyword: "+id->name});
            if (require_unique_names && !names.insert(id->name).second)
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidLetBindings, pr.elems[0]->span(), "duplicate binding: "+id->name});
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
            std::vector<SExprPtr> rebuilt;
            rebuilt.push_back(make_symbol("let", lst.span));
            rebuilt.push_back(make_list({}, pairs.span));
            for (size_t i=2;i<lst.elems.size();++i) rebuilt.push_back(deep_clone(lst.elems[i]));
            auto rebuiltList = make_list(std::move(rebuilt), lst.span);
            return handle_let(*rebuiltList->as<List>());
        }

        // Nest lets
        const auto& firstPair = *pairs.elems[0]->as<List>();
        auto outerBindings = make_list({}, pairs.span);
        outerBindings->as<List>()->elems.push_back([&](){
            auto bp = make_list({}, firstPair.span);
            bp->as<List>()->elems.push_back(deep_clone(firstPair.elems[0]));
            bp->as<List>()->elems.push_back(deep_clone(firstPair.elems[1]));
            return bp;
        }());

        std::vector<SExprPtr> restPairs;
        for (size_t i=1;i<pairs.elems.size();++i) restPairs.push_back(deep_clone(pairs.elems[i]));

        std::vector<SExprPtr> inner;
        inner.push_back(make_symbol("let*", lst.span));
        inner.push_back(make_list(std::move(restPairs), pairs.span));
        for (size_t i=2;i<lst.elems.size();++i) inner.push_back(deep_clone(lst.elems[i]));
        auto innerList = make_list(std::move(inner), lst.span);

        std::vector<SExprPtr> outer;
        outer.push_back(make_symbol("let", lst.span));
        outer.push_back(std::move(outerBindings));
        outer.push_back(std::move(innerList));
        auto outerNode = make_list(std::move(outer), lst.span);
        return handle_let(*outerNode->as<List>());
    }

    ExpanderResult<SExprPtr> Expander::handle_letrec(const List& lst) {
        if (lst.elems.size() < 3 || !lst.elems[1] || !lst.elems[1]->is<List>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "letrec expects bindings and body"});
        const auto& pairs = *lst.elems[1]->as<List>();
        auto parsed = parse_let_pairs(pairs, /*unique*/true); if (!parsed) return std::unexpected(parsed.error());

        // Strategy: allocate placeholders with (let ((x '()) ...)) then (set! x e) ... body
        auto zList = make_list({}, pairs.span);
        for (auto& [id, expr] : *parsed) {
            auto pair = make_list({}, id->span());
            pair->as<List>()->elems.push_back(deep_clone(id));
            pair->as<List>()->elems.push_back(make_nil(id->span()));
            zList->as<List>()->elems.push_back(std::move(pair));
        }

        std::vector<SExprPtr> body;
        for (auto& [id, expr] : *parsed) {
            auto ex = expand_form(expr); if (!ex) return std::unexpected(ex.error());
            auto setp = make_list({}, id->span());
            setp->as<List>()->elems.push_back(make_symbol("set!", id->span()));
            setp->as<List>()->elems.push_back(deep_clone(id));
            setp->as<List>()->elems.push_back(std::move(*ex));
            body.push_back(std::move(setp));
        }
        for (size_t i=2;i<lst.elems.size();++i) { auto r = expand_form(lst.elems[i]); if (!r) return std::unexpected(r.error()); body.push_back(std::move(*r)); }

        auto beginB = make_list({}, lst.span);
        beginB->as<List>()->elems.push_back(make_symbol("begin", lst.span));
        for (auto& b : body) beginB->as<List>()->elems.push_back(std::move(b));

        std::vector<SExprPtr> outer;
        outer.push_back(make_symbol("let", lst.span));
        outer.push_back(std::move(zList));
        outer.push_back(std::move(beginB));
        auto outerList = make_list(std::move(outer), lst.span);
        return handle_let(*outerList->as<List>());
    }

    ExpanderResult<SExprPtr> Expander::handle_letrec_star(const List& lst) {
        // (letrec* ((x e) (y e) ...) body...)
        if (lst.elems.size() < 3 || !lst.elems[1] || !lst.elems[1]->is<List>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "letrec* expects bindings and body"});

        const auto& pairs = *lst.elems[1]->as<List>();

        // No bindings: same as (let () body...)
        if (pairs.elems.empty()) {
            std::vector<SExprPtr> rebuilt;
            rebuilt.push_back(make_symbol("let", lst.span));
            rebuilt.push_back(make_list({}, pairs.span));
            for (size_t i = 2; i < lst.elems.size(); ++i) rebuilt.push_back(deep_clone(lst.elems[i]));
            auto rebuiltList = make_list(std::move(rebuilt), lst.span);
            return handle_let(*rebuiltList->as<List>());
        }

        // Validate pairs and obtain ids/exprs
        auto parsed = parse_let_pairs(pairs, /*require_unique_names*/true);
        if (!parsed) return std::unexpected(parsed.error());

        // Build nested (letrec ((vi ei)) ...) forms to achieve sequential semantics
        // Base continuation is the body as a single begin expression.
        std::function<ExpanderResult<SExprPtr>(std::size_t)> build;
        build = [&](std::size_t idx) -> ExpanderResult<SExprPtr> {
            if (idx >= parsed->size()) {
                std::vector<SExprPtr> beginElems;
                beginElems.push_back(make_symbol("begin", lst.span));
                for (size_t i = 2; i < lst.elems.size(); ++i) beginElems.push_back(deep_clone(lst.elems[i]));
                return make_list(std::move(beginElems), lst.span);
            }

            // Build binding list with a single pair (id expr)
            const auto& id = (*parsed)[idx].first;
            const auto& expr = (*parsed)[idx].second;

            auto pair = make_list({}, id->span());
            pair->as<List>()->elems.push_back(deep_clone(id));
            pair->as<List>()->elems.push_back(deep_clone(expr));

            auto bindingList = make_list({}, pairs.span);
            bindingList->as<List>()->elems.push_back(std::move(pair));

            auto cont = build(idx + 1);
            if (!cont) return std::unexpected(cont.error());

            std::vector<SExprPtr> letrecElems;
            letrecElems.push_back(make_symbol("letrec", lst.span));
            letrecElems.push_back(std::move(bindingList));
            letrecElems.push_back(std::move(*cont));
            auto letrecNode = make_list(std::move(letrecElems), lst.span);
            return handle_letrec(*letrecNode->as<List>());
        };

        return build(0);
    }

    ExpanderResult<SExprPtr> Expander::handle_cond(const List& lst) {
        if (lst.elems.size() < 2)
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "cond expects at least one clause"});

        // Build from the end forward to avoid recursion depth issues
        // Initial "rest" is an empty (begin)
        auto make_empty_begin = [&](Span sp){
            auto v = make_list({}, sp);
            v->as<List>()->elems.push_back(make_symbol("begin", sp));
            return v;
        };

        SExprPtr rest = make_empty_begin(lst.span);
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
                auto b = make_list({}, clause.span);
                b->as<List>()->elems.push_back(make_symbol("begin", clause.span));
                for (size_t k=1;k<clause.elems.size();++k) { auto r = expand_form(clause.elems[k]); if (!r) return std::unexpected(r.error()); b->as<List>()->elems.push_back(std::move(*r)); }
                rest = std::move(b);
                seenElse = true;
                continue;
            }

            auto testX = expand_form(clause.elems[0]); if (!testX) return std::unexpected(testX.error());

            // Arrow clause: (test => proc)
            if (clause.elems.size() == 3 && is_symbol_named(clause.elems[1], "=>")) {
                auto procX = expand_form(clause.elems[2]); if (!procX) return std::unexpected(procX.error());
                auto t = gensym("t");
                auto letBindings = make_list({}, clause.span);
                {
                    auto pair = make_list({}, clause.span);
                    pair->as<List>()->elems.push_back(make_symbol(t, clause.span));
                    pair->as<List>()->elems.push_back(std::move(*testX));
                    letBindings->as<List>()->elems.push_back(std::move(pair));
                }
                auto call = make_list({}, clause.span);
                call->as<List>()->elems.push_back(std::move(*procX));
                call->as<List>()->elems.push_back(make_symbol(t, clause.span));
                auto ifnode = make_list({}, clause.span);
                ifnode->as<List>()->elems.push_back(make_symbol("if", clause.span));
                ifnode->as<List>()->elems.push_back(make_symbol(t, clause.span));
                ifnode->as<List>()->elems.push_back(std::move(call));
                ifnode->as<List>()->elems.push_back(deep_clone(rest));
                auto result = make_list({}, clause.span);
                result->as<List>()->elems.push_back(make_symbol("let", clause.span));
                result->as<List>()->elems.push_back(std::move(letBindings));
                result->as<List>()->elems.push_back(std::move(ifnode));
                rest = std::move(result);
                continue;
            }

            // Standard clause: (test expr...)
            if (clause.elems.size() == 1) {
                // (if test test rest)
                auto out = make_list({}, clause.span);
                out->as<List>()->elems.push_back(make_symbol("if", clause.span));
                out->as<List>()->elems.push_back(std::move(*testX));
                out->as<List>()->elems.push_back(deep_clone(clause.elems[0]));
                out->as<List>()->elems.push_back(std::move(rest));
                rest = std::move(out);
                continue;
            }

            auto b = make_list({}, clause.span);
            b->as<List>()->elems.push_back(make_symbol("begin", clause.span));
            for (size_t k=1;k<clause.elems.size();++k) { auto r = expand_form(clause.elems[k]); if (!r) return std::unexpected(r.error()); b->as<List>()->elems.push_back(std::move(*r)); }
            auto out = make_list({}, clause.span);
            out->as<List>()->elems.push_back(make_symbol("if", clause.span));
            out->as<List>()->elems.push_back(std::move(*testX));
            out->as<List>()->elems.push_back(std::move(b));
            out->as<List>()->elems.push_back(std::move(rest));
            rest = std::move(out);
        }

        return rest;
    }

    ExpanderResult<SExprPtr> Expander::handle_module_list(const List& lst) {
        // (module name form...)
        if (lst.elems.size() < 2 || !lst.elems[1] || !lst.elems[1]->is<Symbol>())
            return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, lst.span, "module expects (module name forms...)"});

        // Validate body forms: only define, export, import
        for (size_t i=2;i<lst.elems.size();++i) {
            const auto& f = lst.elems[i];
            if (!f || !f->is<List>())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, f ? f->span() : lst.span, "invalid form in module body; allowed: define, export, import"});
            const auto& l = *f->as<List>();
            if (l.elems.empty() || !l.elems[0] || !l.elems[0]->is<Symbol>())
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, l.span, "invalid form in module body; allowed: define, export, import"});
            const auto* h = l.elems[0]->as<Symbol>();
            if (!(h->name == "define" || h->name == "export" || h->name == "import"))
                return std::unexpected(ExpandError{ExpandError::Kind::InvalidSyntax, l.span, "invalid form in module body; allowed: define, export, import"});
        }

        std::vector<SExprPtr> xs; xs.reserve(lst.elems.size());
        xs.push_back(make_symbol("module", lst.span));
        xs.push_back(deep_clone(lst.elems[1]));
        for (size_t i=2;i<lst.elems.size();++i) { auto r = expand_form(lst.elems[i]); if (!r) return std::unexpected(r.error()); xs.push_back(std::move(*r)); }
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

}
