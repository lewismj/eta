### Overview
This is a complete, self-contained solution sketch for a Scheme-like expander with:
- A two-phase module system (Expand → Link) supporting circular imports, `export`/`import` with `only`/`except`/`rename`.
- Robust `lambda` formals parsing (proper, dotted, and rest-only) with duplicate and reserved keyword checks.
- `let`, `let*`, `letrec`, and named `let` with correct desugaring and validation.
- `cond` with `=>` and `else`.
- Reserved keyword protection for params, let-bindings, defines, and imported names.
- Clean separation between compilation (reader/expander/linker/emitter) and runtime (VM + runtime types).

The code is presented as two main files you can adapt to your project structure:
- `expander.h` — public front-end API and core data types for expansion and linking.
- `expander.cpp` — full implementation of the expander and linker.

Additionally, a small `emitter.h` sketch shows how to keep a constant pool and symbol interning separate from runtime types. Test snippets and an integration checklist are included at the end.


### expander.h (full)
```cpp
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <utility>
#include <expected>
#include <memory>

namespace eta::expand {

// ---- Core front-end nodes (adapt to your AST) ----
struct Span {
  // Fill with your file/line/column fields
};

struct SExpr { // Base node; your project likely has variants derived from this
  virtual ~SExpr() = default;
  virtual Span span() const = 0;
};

using SExprPtr = std::shared_ptr<SExpr>;

struct Symbol : SExpr {
  std::string name;
  Span sp;
  Span span() const override { return sp; }
};

struct List : SExpr {
  std::vector<SExprPtr> elems;
  Span sp;
  Span span() const override { return sp; }
};

// ---- Factories (replace with your constructors) ----
SExprPtr sym(const std::string& name, Span s);
SExprPtr list(std::vector<SExprPtr> elems, Span s);
SExprPtr nil_lit(Span s);
bool     is_symbol(const SExprPtr& e);
bool     is_list(const SExprPtr& e);
bool     is_dot(const SExprPtr& e); // true if reader produced dedicated dot token; else treat symbol(".") as dot
const Symbol& as_symbol(const SExprPtr& e);
const List&   as_list(const SExprPtr& e);

// ---- Errors ----
enum class ExpandErrorKind {
  InvalidSyntax,
  UndefinedIdentifier,
  RedefinedIdentifier,
  DuplicateIdentifier,
  ImportError,
  ExportError,
  ModuleError,
  InvalidLetBindings,
};

struct ExpandError {
  ExpandErrorKind kind{};
  Span span{};
  std::string message;
};

template <typename T>
using XResult = std::expected<T, ExpandError>;

// ---- Lexical bindings ----
struct Binding {
  std::string name;
  Span span{};
};

struct Env {
  Env* parent{nullptr};
  std::unordered_map<std::string, Binding> frame;

  bool defines_in_frame(const std::string& id) const;
  bool define_here(const std::string& id, Span s, ExpandError* errIfConflict = nullptr);
  const Binding* lookup(const std::string& id) const; // parent-chain lookup
};

// ---- Module system types ----
struct ExportSet {
  std::vector<std::string> idents; // exported ids
  Span span{};
};

struct ImportSet {
  enum class Kind { Plain, Only, Except, Rename };
  Kind kind{Kind::Plain};
  std::string module_name; // e.g. "std.core"
  std::vector<std::string> idents; // for only/except
  std::vector<std::pair<std::string,std::string>> renames; // (old -> new)
  Span span{};
};

struct ModuleEnv {
  std::string name;
  std::unordered_map<std::string, Binding> table; // module-local names (defs + imported aliases post-link)
  std::unordered_set<std::string> exports;        // names to be exported
};

// ---- Two-phase compilation context ----
struct CompilationUnit {
  std::unordered_map<std::string, ModuleEnv> modules;

  enum class ModuleState { NotStarted, Expanding, Expanded };
  std::unordered_map<std::string, ModuleState> module_states;

  struct PendingImport {
    std::string target_module; // module receiving the import (where clause occurred)
    ImportSet   spec;
    Span        where;
  };

  std::unordered_map<std::string, std::vector<PendingImport>> pending_imports; // keyed by target module name

  // Optional error sink if you want to accumulate instead of early return
  std::vector<ExpandError> errors;
};

// ---- Lambda formals ----
struct Formals {
  std::vector<std::string> fixed;
  std::optional<std::string> rest;
  Span span{};
};

// ---- Expander ----
class Expander {
public:
  Expander();

  // Expand a form (SExprPtr in, SExprPtr out); atoms pass through, lists normalized
  XResult<SExprPtr> expand_form(const SExprPtr& e);

  // Expand a sequence of forms
  XResult<std::vector<SExprPtr>> expand_many(const std::vector<SExprPtr>& forms);

  // Convenience: expand then link all modules referenced, returns expanded forms
  XResult<std::vector<SExprPtr>> expand_and_link(const std::vector<SExprPtr>& forms);

private:
  // Current lexical env
  Env* env_{};
  Env global_{};

  // Current module during expansion (nullptr if not in a module)
  ModuleEnv* current_module_{nullptr};

  // Shared compilation context (registry, states, pending imports)
  CompilationUnit cu_{};

  // RAII guards
  struct EnvGuard { Env*& cur; Env* saved; EnvGuard(Env*& r, Env* next):cur(r),saved(r){cur=next;} ~EnvGuard(){cur=saved;} };
  struct ModuleGuard { ModuleEnv*& cur; ModuleEnv* saved; ModuleGuard(ModuleEnv*& r, ModuleEnv* next):cur(r),saved(r){cur=next;} ~ModuleGuard(){cur=saved;} };

  // Dispatch for applications
  XResult<SExprPtr> expand_application(const List& app);

  // Handlers for special/derived forms
  XResult<SExprPtr> handle_quote(const List& lst);
  XResult<SExprPtr> handle_if(const List& lst);
  XResult<SExprPtr> handle_begin(const List& lst);
  XResult<SExprPtr> handle_define(const List& lst);
  XResult<SExprPtr> handle_set_bang(const List& lst);

  XResult<SExprPtr> handle_lambda(const List& lst);
  XResult<SExprPtr> handle_let(const List& lst);
  XResult<SExprPtr> handle_let_star(const List& lst);
  XResult<SExprPtr> handle_letrec(const List& lst);
  XResult<SExprPtr> handle_cond(const List& lst);

  XResult<SExprPtr> handle_module(const List& lst);
  XResult<SExprPtr> handle_export(const List& lst);
  XResult<SExprPtr> handle_import(const List& lst);

  // Convenience sugar
  XResult<SExprPtr> handle_defun(const List& lst); // (defun name (args) body...) -> (define name (lambda ...))
  XResult<SExprPtr> handle_progn(const List& lst); // synonym for begin

  // Helpers
  XResult<Formals> parse_formals_node(const SExprPtr& node);
  XResult<std::vector<Binding>> parse_let_bindings(const List& pair_list, bool require_unique_within);
  XResult<ImportSet> parse_import_set(const List& lst);

  // Desugars
  XResult<SExprPtr> desugar_let(const List& lst);
  XResult<SExprPtr> desugar_let_star(const List& lst);
  XResult<SExprPtr> desugar_letrec(const List& lst);
  XResult<SExprPtr> desugar_named_let(const List& lst);

  // Utilities
  bool is_named_let_syntax(const List& lst) const;
  bool is_reserved_keyword(const std::string& name) const;
  std::string gensym(const std::string& hint = "g");

  // Link phase
  XResult<void> link_all_modules();

  // Error helper
  ExpandError err(ExpandErrorKind k, Span s, const std::string& msg) const;
};

} // namespace eta::expand
```


### expander.cpp (full)
```cpp
#include "expander.h"
#include <sstream>
#include <functional>

namespace eta::expand {

// ----- Utility predicates (adjust to your AST) -----
static bool is_symbol_named(const SExprPtr& e, const char* name) {
  return is_symbol(e) && as_symbol(e).name == name;
}

static bool is_core_dot(const SExprPtr& e) {
  if (is_dot(e)) return true;         // reader provides a distinct DOT token
  return is_symbol_named(e, ".");   // fallback: symbol(".")
}

// Reserved keywords to protect from ordinary rebinding
static const std::unordered_set<std::string>& reserved_keywords() {
  static const std::unordered_set<std::string> K = {
    "quote","if","begin","lambda","define","set!",
    "let","let*","letrec","cond","and","or",
    "quasiquote","unquote","unquote-splicing",
    "module","import","export","case","do","when","unless"
  };
  return K;
}

bool Expander::is_reserved_keyword(const std::string& name) const {
  return reserved_keywords().count(name) != 0;
}

// ---- Env impl ----
bool Env::defines_in_frame(const std::string& id) const {
  return frame.find(id) != frame.end();
}

bool Env::define_here(const std::string& id, Span s, ExpandError* errIfConflict) {
  if (defines_in_frame(id)) {
    if (errIfConflict) {
      *errIfConflict = ExpandError{ExpandErrorKind::RedefinedIdentifier, s, "duplicate binding: "+id};
    }
    return false;
  }
  frame.emplace(id, Binding{id, s});
  return true;
}

const Binding* Env::lookup(const std::string& id) const {
  auto it = frame.find(id);
  if (it != frame.end()) return &it->second;
  if (parent) return parent->lookup(id);
  return nullptr;
}

// ---- Expander ctor ----
Expander::Expander() {
  env_ = &global_;
}

ExpandError Expander::err(ExpandErrorKind k, Span s, const std::string& msg) const {
  return ExpandError{k, s, msg};
}

// ---- Public API ----
XResult<std::vector<SExprPtr>> Expander::expand_many(const std::vector<SExprPtr>& forms) {
  std::vector<SExprPtr> out; out.reserve(forms.size());
  for (auto& f : forms) {
    auto r = expand_form(f);
    if (!r) return std::unexpected(r.error());
    out.push_back(std::move(*r));
  }
  return out;
}

XResult<std::vector<SExprPtr>> Expander::expand_and_link(const std::vector<SExprPtr>& forms) {
  auto expanded = expand_many(forms);
  if (!expanded) return std::unexpected(expanded.error());
  auto linkr = link_all_modules();
  if (!linkr) return std::unexpected(linkr.error());
  return expanded;
}

// ---- Expand dispatch ----
XResult<SExprPtr> Expander::expand_form(const SExprPtr& e) {
  if (!is_list(e)) {
    // Symbol or literal: return as-is (you may normalize identifiers if needed)
    return e;
  }
  const auto& lst = as_list(e);
  return expand_application(lst);
}

XResult<SExprPtr> Expander::expand_application(const List& lst) {
  if (lst.elems.empty()) return list({}, lst.sp);

  auto head = lst.elems[0];
  if (!is_symbol(head)) {
    // General application: expand all elements
    std::vector<SExprPtr> xs; xs.reserve(lst.elems.size());
    for (auto& a : lst.elems) { auto r = expand_form(a); if (!r) return std::unexpected(r.error()); xs.push_back(*r);} 
    return list(std::move(xs), lst.sp);
  }

  const auto& h = as_symbol(head).name;

  if (h == "quote")   return handle_quote(lst);
  if (h == "if")      return handle_if(lst);
  if (h == "begin")   return handle_begin(lst);
  if (h == "lambda")  return handle_lambda(lst);
  if (h == "define")  return handle_define(lst);
  if (h == "set!")    return handle_set_bang(lst);

  if (h == "let")     return handle_let(lst);
  if (h == "let*")    return handle_let_star(lst);
  if (h == "letrec")  return handle_letrec(lst);
  if (h == "cond")    return handle_cond(lst);

  if (h == "module")  return handle_module(lst);
  if (h == "export")  return handle_export(lst);
  if (h == "import")  return handle_import(lst);

  if (h == "defun")   return handle_defun(lst);
  if (h == "progn")   return handle_progn(lst);

  // Default: general application
  std::vector<SExprPtr> xs; xs.reserve(lst.elems.size());
  for (auto& a : lst.elems) { auto r = expand_form(a); if (!r) return std::unexpected(r.error()); xs.push_back(*r); }
  return list(std::move(xs), lst.sp);
}

// ---- Core forms ----
XResult<SExprPtr> Expander::handle_quote(const List& lst) {
  if (lst.elems.size() != 2)
    return std::unexpected(err(ExpandErrorKind::InvalidSyntax, lst.sp, "quote expects 1 arg"));
  // Return as-is `(quote datum)`
  return list({ lst.elems[0], lst.elems[1] }, lst.sp);
}

XResult<SExprPtr> Expander::handle_if(const List& lst) {
  if (lst.elems.size() < 3 || lst.elems.size() > 4)
    return std::unexpected(err(ExpandErrorKind::InvalidSyntax, lst.sp, "if expects 2 or 3 args"));
  std::vector<SExprPtr> xs; xs.reserve(lst.elems.size());
  xs.push_back(lst.elems[0]);
  for (size_t i=1;i<lst.elems.size();++i) { auto r = expand_form(lst.elems[i]); if (!r) return std::unexpected(r.error()); xs.push_back(*r);} 
  return list(std::move(xs), lst.sp);
}

XResult<SExprPtr> Expander::handle_begin(const List& lst) {
  std::vector<SExprPtr> xs; xs.reserve(lst.elems.size());
  xs.push_back(lst.elems[0]);
  for (size_t i=1;i<lst.elems.size();++i) { auto r = expand_form(lst.elems[i]); if (!r) return std::unexpected(r.error()); xs.push_back(*r); }
  return list(std::move(xs), lst.sp);
}

XResult<SExprPtr> Expander::handle_define(const List& lst) {
  if (lst.elems.size() < 3)
    return std::unexpected(err(ExpandErrorKind::InvalidSyntax, lst.sp, "define expects name and value"));

  auto target = lst.elems[1];
  if (is_symbol(target)) {
    auto id = as_symbol(target).name;
    if (is_reserved_keyword(id))
      return std::unexpected(err(ExpandErrorKind::InvalidSyntax, target->span(), "cannot define reserved keyword: "+id));

    ExpandError e{};
    if (!env_->define_here(id, target->span(), &e)) return std::unexpected(e);
    // Record in module table for intra-module refs + exports validation
    if (current_module_) current_module_->table.emplace(id, Binding{ id, target->span() });

    auto rhs = expand_form(lst.elems[2]); if (!rhs) return std::unexpected(rhs.error());
    return list({ lst.elems[0], target, *rhs }, lst.sp);
  }

  // Function sugar: (define (f args...) body...) → (define f (lambda (args...) body...))
  if (!is_list(target))
    return std::unexpected(err(ExpandErrorKind::InvalidSyntax, target->span(), "bad define head"));

  const auto& sig = as_list(target);
  if (sig.elems.empty() || !is_symbol(sig.elems[0]))
    return std::unexpected(err(ExpandErrorKind::InvalidSyntax, target->span(), "bad define head"));

  auto fname = as_symbol(sig.elems[0]).name;
  if (is_reserved_keyword(fname))
    return std::unexpected(err(ExpandErrorKind::InvalidSyntax, sig.elems[0]->span(), "cannot define reserved keyword: "+fname));

  ExpandError e{};
  if (!env_->define_here(fname, sig.elems[0]->span(), &e)) return std::unexpected(e);
  if (current_module_) current_module_->table.emplace(fname, Binding{ fname, sig.elems[0]->span() });

  // Build lambda node
  std::vector<SExprPtr> lam; lam.push_back(sym("lambda", target->span()));
  {
    std::vector<SExprPtr> params;
    for (size_t i=1;i<sig.elems.size();++i) params.push_back(sig.elems[i]);
    lam.push_back(list(std::move(params), target->span()));
  }
  for (size_t i=2;i<lst.elems.size();++i) lam.push_back(lst.elems[i]);

  auto lamE = list(std::move(lam), lst.sp);
  auto lamX = handle_lambda(as_list(lamE)); if (!lamX) return std::unexpected(lamX.error());
  return list({ lst.elems[0], sym(fname, sig.elems[0]->span()), *lamX }, lst.sp);
}

XResult<SExprPtr> Expander::handle_set_bang(const List& lst) {
  if (lst.elems.size() != 3 || !is_symbol(lst.elems[1]))
    return std::unexpected(err(ExpandErrorKind::InvalidSyntax, lst.sp, "set! expects (set! id expr)"));
  auto rhs = expand_form(lst.elems[2]); if (!rhs) return std::unexpected(rhs.error());
  return list({ lst.elems[0], lst.elems[1], *rhs }, lst.sp);
}

// ---- Lambda & formals ----
XResult<Formals> Expander::parse_formals_node(const SExprPtr& p) {
  Span s = p->span();
  Formals f; f.span = s;
  std::unordered_set<std::string> seen;
  auto dup = [&](const std::string& n, Span sp) -> XResult<Formals> {
    return std::unexpected(err(ExpandErrorKind::DuplicateIdentifier, sp, "duplicate parameter: "+n));
  };

  if (is_symbol(p)) {
    auto n = as_symbol(p).name;
    if (is_reserved_keyword(n))
      return std::unexpected(err(ExpandErrorKind::InvalidSyntax, s, "reserved keyword as rest parameter: "+n));
    f.rest = n;
    return f;
  }
  if (!is_list(p))
    return std::unexpected(err(ExpandErrorKind::InvalidSyntax, s, "lambda formals must be a symbol or list"));

  const auto& lst = as_list(p);
  for (size_t i=0;i<lst.elems.size();++i) {
    if (is_core_dot(lst.elems[i])) {
      if (i+1 >= lst.elems.size() || !is_symbol(lst.elems[i+1]))
        return std::unexpected(err(ExpandErrorKind::InvalidSyntax, s, "'.' must be followed by a symbol"));
      if (i+2 != lst.elems.size())
        return std::unexpected(err(ExpandErrorKind::InvalidSyntax, s, "'.' must be penultimate in formals"));
      auto restn = as_symbol(lst.elems[i+1]).name;
      if (is_reserved_keyword(restn))
        return std::unexpected(err(ExpandErrorKind::InvalidSyntax, lst.elems[i+1]->span(), "reserved keyword as parameter: "+restn));
      if (!seen.insert(restn).second) return dup(restn, lst.elems[i+1]->span());
      f.rest = restn;
      return f; // dotted tail ends the list
    }
    if (!is_symbol(lst.elems[i]))
      return std::unexpected(err(ExpandErrorKind::InvalidSyntax, lst.elems[i]->span(), "parameter must be a symbol"));
    auto n = as_symbol(lst.elems[i]).name;
    if (is_reserved_keyword(n))
      return std::unexpected(err(ExpandErrorKind::InvalidSyntax, lst.elems[i]->span(), "reserved keyword as parameter: "+n));
    if (!seen.insert(n).second) return dup(n, lst.elems[i]->span());
    f.fixed.push_back(n);
  }
  return f;
}

XResult<SExprPtr> Expander::handle_lambda(const List& lst) {
  if (lst.elems.size() < 3)
    return std::unexpected(err(ExpandErrorKind::InvalidSyntax, lst.sp, "lambda expects formals and body"));

  auto FM = parse_formals_node(lst.elems[1]);
  if (!FM) return std::unexpected(FM.error());
  const auto& f = *FM;

  // New lexical frame for params
  Env fnEnv; fnEnv.parent = env_;
  EnvGuard guard(env_, &fnEnv);

  for (auto& n : f.fixed) { ExpandError e{}; if (!env_->define_here(n, lst.elems[1]->span(), &e)) return std::unexpected(e); }
  if (f.rest) { ExpandError e{}; if (!env_->define_here(*f.rest, lst.elems[1]->span(), &e)) return std::unexpected(e); }

  // Expand body
  std::vector<SExprPtr> out; out.reserve(lst.elems.size());
  out.push_back(lst.elems[0]);

  // Rebuild normalized formals: keep original node if you prefer; here we normalize list form
  if (is_symbol(lst.elems[1])) {
    out.push_back(lst.elems[1]);
  } else {
    std::vector<SExprPtr> params;
    for (auto& n : f.fixed) params.push_back(sym(n, lst.elems[1]->span()));
    if (f.rest) { params.push_back(sym(".", lst.elems[1]->span())); params.push_back(sym(*f.rest, lst.elems[1]->span())); }
    out.push_back(list(std::move(params), lst.elems[1]->span()));
  }

  for (size_t i=2;i<lst.elems.size();++i) { auto r = expand_form(lst.elems[i]); if (!r) return std::unexpected(r.error()); out.push_back(*r);} 
  return list(std::move(out), lst.sp);
}

// ---- let family ----
XResult<std::vector<Binding>> Expander::parse_let_bindings(const List& pair_list, bool require_unique_within) {
  std::vector<Binding> out;
  std::unordered_set<std::string> names;
  for (auto& p : pair_list.elems) {
    if (!is_list(p)) return std::unexpected(err(ExpandErrorKind::InvalidLetBindings, p->span(), "binding must be (id expr)"));
    const auto& pr = as_list(p);
    if (pr.elems.size() != 2 || !is_symbol(pr.elems[0]))
      return std::unexpected(err(ExpandErrorKind::InvalidLetBindings, pr.span, "binding must be (id expr)"));
    auto id = as_symbol(pr.elems[0]).name;
    if (is_reserved_keyword(id))
      return std::unexpected(err(ExpandErrorKind::InvalidLetBindings, pr.elems[0]->span(), "cannot bind reserved keyword: "+id));
    if (require_unique_within && !names.insert(id).second)
      return std::unexpected(err(ExpandErrorKind::InvalidLetBindings, pr.elems[0]->span(), "duplicate binding: "+id));
    out.push_back(Binding{ id, pr.span });
  }
  return out;
}

bool Expander::is_named_let_syntax(const List& lst) const {
  // (let name ((x e) ...) body...)
  return lst.elems.size() >= 3 && is_symbol(lst.elems[1]) && is_list(lst.elems[2]);
}

XResult<SExprPtr> Expander::desugar_let(const List& lst) {
  if (lst.elems.size() < 3 || !is_list(lst.elems[1]))
    return std::unexpected(err(ExpandErrorKind::InvalidSyntax, lst.sp, "let expects bindings and body"));

  const auto& pairs = as_list(lst.elems[1]);
  auto bindsR = parse_let_bindings(pairs, /*unique*/true);
  if (!bindsR) return std::unexpected(bindsR.error());

  // Collect params and expanded inits
  std::vector<SExprPtr> params; params.reserve(pairs.elems.size());
  std::vector<SExprPtr> inits;  inits.reserve(pairs.elems.size());
  for (auto& p : pairs.elems) {
    const auto& pr = as_list(p);
    params.push_back(pr.elems[0]);
    auto r = expand_form(pr.elems[1]); if (!r) return std::unexpected(r.error());
    inits.push_back(*r);
  }

  // Build (lambda (params) body...)
  std::vector<SExprPtr> lam; lam.push_back(sym("lambda", lst.sp));
  lam.push_back(list(std::move(params), lst.elems[1]->span()));
  for (size_t i=2;i<lst.elems.size();++i) { auto r = expand_form(lst.elems[i]); if (!r) return std::unexpected(r.error()); lam.push_back(*r);} 
  auto lamE = list(std::move(lam), lst.sp);
  auto lamX = handle_lambda(as_list(lamE)); if (!lamX) return std::unexpected(lamX.error());

  // Application: ((lambda ...) inits...)
  std::vector<SExprPtr> app; app.push_back(*lamX);
  for (auto& e : inits) app.push_back(e);
  return list(std::move(app), lst.sp);
}

XResult<SExprPtr> Expander::desugar_let_star(const List& lst) {
  if (lst.elems.size() < 3 || !is_list(lst.elems[1]))
    return std::unexpected(err(ExpandErrorKind::InvalidSyntax, lst.sp, "let* expects bindings and body"));
  const auto& pairs = as_list(lst.elems[1]);
  if (pairs.elems.empty()) {
    // (let* () body...) == (let () body...)
    std::vector<SExprPtr> rebuilt{ sym("let", lst.sp), list({}, lst.elems[1]->span()) };
    for (size_t i=2;i<lst.elems.size();++i) rebuilt.push_back(lst.elems[i]);
    return desugar_let(as_list(list(std::move(rebuilt), lst.sp)));
  }

  // Nest: take first, then recurse
  auto firstPair = as_list(pairs.elems[0]);
  std::vector<SExprPtr> outerBindings{ list({ firstPair.elems[0], firstPair.elems[1] }, firstPair.span) };
  auto outerBindingList = list(std::move(outerBindings), pairs.span);

  std::vector<SExprPtr> restPairs;
  for (size_t i=1;i<pairs.elems.size();++i) restPairs.push_back(pairs.elems[i]);
  std::vector<SExprPtr> inner{ sym("let*", lst.sp), list(std::move(restPairs), pairs.span) };
  for (size_t i=2;i<lst.elems.size();++i) inner.push_back(lst.elems[i]);

  std::vector<SExprPtr> outer{ sym("let", lst.sp), outerBindingList, list(std::move(inner), lst.sp) };
  return desugar_let(as_list(list(std::move(outer), lst.sp)));
}

XResult<SExprPtr> Expander::desugar_letrec(const List& lst) {
  if (lst.elems.size() < 3 || !is_list(lst.elems[1]))
    return std::unexpected(err(ExpandErrorKind::InvalidSyntax, lst.sp, "letrec expects bindings and body"));
  const auto& pairs = as_list(lst.elems[1]);
  auto bindsR = parse_let_bindings(pairs, /*unique*/true);
  if (!bindsR) return std::unexpected(bindsR.error());

  // Allocate placeholders, then set!, then body
  std::vector<SExprPtr> zeroPairs;
  for (auto& p : pairs.elems) {
    const auto& pr = as_list(p);
    zeroPairs.push_back(list({ pr.elems[0], sym("Nil", pr.span) }, pr.span)); // or an implementation-defined placeholder
  }
  auto zList = list(std::move(zeroPairs), pairs.span);

  std::vector<SExprPtr> body;
  for (auto& p : pairs.elems) {
    const auto& pr = as_list(p);
    auto r = expand_form(pr.elems[1]); if (!r) return std::unexpected(r.error());
    body.push_back(list({ sym("set!", pr.span), pr.elems[0], *r }, pr.span));
  }
  for (size_t i=2;i<lst.elems.size();++i) { auto r = expand_form(lst.elems[i]); if (!r) return std::unexpected(r.error()); body.push_back(*r);} 

  std::vector<SExprPtr> beginB; beginB.push_back(sym("begin", lst.sp));
  beginB.insert(beginB.end(), body.begin(), body.end());
  auto bodyBegin = list(std::move(beginB), lst.sp);

  std::vector<SExprPtr> outer{ sym("let", lst.sp), zList, bodyBegin };
  return desugar_let(as_list(list(std::move(outer), lst.sp)));
}

XResult<SExprPtr> Expander::desugar_named_let(const List& lst) {
  // (let name ((x e) ...) body...) → (letrec ((name (lambda (x ...) body...))) (name e ...))
  auto nameSym = lst.elems[1];
  const auto& pairs = as_list(lst.elems[2]);

  std::vector<SExprPtr> params;
  std::vector<SExprPtr> inits;
  for (auto& p : pairs.elems) {
    const auto& pr = as_list(p);
    params.push_back(pr.elems[0]);
    auto r = expand_form(pr.elems[1]); if (!r) return std::unexpected(r.error());
    inits.push_back(*r);
  }

  std::vector<SExprPtr> lam; lam.push_back(sym("lambda", lst.sp));
  lam.push_back(list(std::move(params), pairs.span));
  for (size_t i=3;i<lst.elems.size();++i) { auto r = expand_form(lst.elems[i]); if (!r) return std::unexpected(r.error()); lam.push_back(*r);} 
  auto lamE = list(std::move(lam), lst.sp);
  auto lamX = handle_lambda(as_list(lamE)); if (!lamX) return std::unexpected(lamX.error());

  auto binding = list({ nameSym, *lamX }, lst.sp);
  auto bindingList = list({ binding }, lst.sp);
  std::vector<SExprPtr> call; call.push_back(nameSym); for (auto& a : inits) call.push_back(a);

  std::vector<SExprPtr> letrecForm{ sym("letrec", lst.sp), bindingList, list(std::move(call), lst.sp) };
  return desugar_letrec(as_list(list(std::move(letrecForm), lst.sp)));
}

XResult<SExprPtr> Expander::handle_let(const List& lst) {
  if (is_named_let_syntax(lst)) return desugar_named_let(lst);
  return desugar_let(lst);
}

XResult<SExprPtr> Expander::handle_let_star(const List& lst) { return desugar_let_star(lst); }
XResult<SExprPtr> Expander::handle_letrec(const List& lst)   { return desugar_letrec(lst); }

// ---- cond with => and else ----
XResult<SExprPtr> Expander::handle_cond(const List& lst) {
  if (lst.elems.size() < 2)
    return std::unexpected(err(ExpandErrorKind::InvalidSyntax, lst.sp, "cond expects clauses"));

  std::function<XResult<SExprPtr>(size_t)> loop = [&](size_t i)->XResult<SExprPtr> {
    if (i >= lst.elems.size())
      return list({ sym("begin", lst.sp) }, lst.sp); // no-op

    if (!is_list(lst.elems[i]))
      return std::unexpected(err(ExpandErrorKind::InvalidSyntax, lst.elems[i]->span(), "cond clause must be a list"));

    const auto& clause = as_list(lst.elems[i]);
    if (clause.elems.empty())
      return std::unexpected(err(ExpandErrorKind::InvalidSyntax, clause.span, "empty cond clause"));

    if (is_symbol_named(clause.elems[0], "else")) {
      if (i+1 != lst.elems.size())
        return std::unexpected(err(ExpandErrorKind::InvalidSyntax, clause.span, "else must be last"));
      std::vector<SExprPtr> b{ sym("begin", clause.span) };
      for (size_t k=1;k<clause.elems.size();++k) { auto r = expand_form(clause.elems[k]); if (!r) return std::unexpected(r.error()); b.push_back(*r);} 
      return list(std::move(b), clause.span);
    }

    auto testX = expand_form(clause.elems[0]); if (!testX) return std::unexpected(testX.error());

    // Arrow clause: (test => proc)
    if (clause.elems.size() == 3 && is_symbol_named(clause.elems[1], "=>")) {
      auto procX = expand_form(clause.elems[2]); if (!procX) return std::unexpected(procX.error());
      auto t = gensym("t");
      auto letBindings = list({ list({ sym(t, clause.span), *testX }, clause.span) }, clause.span);
      auto rest = loop(i+1); if (!rest) return std::unexpected(rest.error());
      auto call = list({ *procX, sym(t, clause.span) }, clause.span);
      auto ifnode = list({ sym("if", clause.span), sym(t, clause.span), call, *rest }, clause.span);
      return list({ sym("let", clause.span), letBindings, ifnode }, clause.span);
    }

    // Standard clause: (test expr...)
    std::vector<SExprPtr> b{ sym("begin", clause.span) };
    for (size_t k=1;k<clause.elems.size();++k) { auto r = expand_form(clause.elems[k]); if (!r) return std::unexpected(r.error()); b.push_back(*r);} 
    auto rest = loop(i+1); if (!rest) return std::unexpected(rest.error());
    return list({ sym("if", clause.span), *testX, list(std::move(b), clause.span), *rest }, clause.span);
  };

  return loop(1);
}

// ---- Module system (two-phase) ----
XResult<SExprPtr> Expander::handle_module(const List& lst) {
  // (module Name form...)
  if (lst.elems.size() < 2 || !is_symbol(lst.elems[1]))
    return std::unexpected(err(ExpandErrorKind::InvalidSyntax, lst.sp, "module expects (module name forms...)"));

  auto name = as_symbol(lst.elems[1]).name;
  auto& mod = cu_.modules[name];
  if (mod.name.empty()) mod.name = name;
  cu_.module_states[name] = CompilationUnit::ModuleState::Expanding;

  // Fresh lexical env for module body
  Env moduleLex; moduleLex.parent = &global_;
  ModuleGuard mguard(current_module_, &mod);
  EnvGuard     eguard(env_, &moduleLex);

  // Expand body
  std::vector<SExprPtr> body;
  for (size_t i=2;i<lst.elems.size();++i) { auto r = expand_form(lst.elems[i]); if (!r) return std::unexpected(r.error()); body.push_back(*r);} 

  cu_.module_states[name] = CompilationUnit::ModuleState::Expanded;

  // Build a ModuleForm list if you lack a dedicated node: (ModuleForm name (begin ...))
  std::vector<SExprPtr> beginForm{ sym("begin", lst.sp) };
  beginForm.insert(beginForm.end(), body.begin(), body.end());
  auto bodyBegin = list(std::move(beginForm), lst.sp);
  return list({ sym("ModuleForm", lst.sp), sym(name, lst.elems[1]->span()), bodyBegin }, lst.sp);
}

XResult<SExprPtr> Expander::handle_export(const List& lst) {
  if (!current_module_) return std::unexpected(err(ExpandErrorKind::ExportError, lst.sp, "export only valid inside a module"));
  if (lst.elems.size() < 2) return std::unexpected(err(ExpandErrorKind::ExportError, lst.sp, "export expects at least one identifier"));
  for (size_t i=1;i<lst.elems.size();++i) {
    if (!is_symbol(lst.elems[i])) return std::unexpected(err(ExpandErrorKind::ExportError, lst.elems[i]->span(), "export expects identifiers"));
    auto id = as_symbol(lst.elems[i]).name;
    if (!current_module_->exports.insert(id).second)
      return std::unexpected(err(ExpandErrorKind::ExportError, lst.elems[i]->span(), "duplicate export: "+id));
  }
  // Directive no-op
  return list({ sym("begin", lst.sp) }, lst.sp);
}

XResult<ImportSet> Expander::parse_import_set(const List& lst) {
  // Accept: (m) | (only (m) ids...) | (except (m) ids...) | (rename (m) (old new) ...)
  if (lst.elems.empty()) return std::unexpected(err(ExpandErrorKind::ImportError, lst.span(), "empty import clause"));

  auto read_modname = [&](const SExprPtr& p)->XResult<std::string>{
    if (is_symbol(p)) return as_symbol(p).name; // allow bare symbol as shorthand
    if (!is_list(p)) return std::unexpected(err(ExpandErrorKind::ImportError, p->span(), "module spec must be (name) or name"));
    const auto& mod = as_list(p);
    if (mod.elems.size()!=1 || !is_symbol(mod.elems[0]))
      return std::unexpected(err(ExpandErrorKind::ImportError, p->span(), "module spec must be (name)"));
    return as_symbol(mod.elems[0]).name;
  };

  if (is_symbol(lst.elems[0]) && lst.elems.size()==1) {
    ImportSet s; s.kind = ImportSet::Kind::Plain; s.module_name = as_symbol(lst.elems[0]).name; s.span = lst.sp; return s;
  }

  if (!is_symbol(lst.elems[0]))
    return std::unexpected(err(ExpandErrorKind::ImportError, lst.elems[0]->span(), "bad import clause head"));

  auto head = as_symbol(lst.elems[0]).name;

  if (head == "only" || head == "except") {
    if (lst.elems.size() < 3)
      return std::unexpected(err(ExpandErrorKind::ImportError, lst.sp, head+" expects module and names"));
    auto m = read_modname(lst.elems[1]); if (!m) return std::unexpected(m.error());
    ImportSet s; s.kind = (head=="only"? ImportSet::Kind::Only : ImportSet::Kind::Except); s.module_name = *m; s.span = lst.sp;
    for (size_t i=2;i<lst.elems.size();++i) {
      if (!is_symbol(lst.elems[i])) return std::unexpected(err(ExpandErrorKind::ImportError, lst.elems[i]->span(), "identifier expected"));
      s.idents.push_back(as_symbol(lst.elems[i]).name);
    }
    return s;
  }

  if (head == "rename") {
    if (lst.elems.size() < 3)
      return std::unexpected(err(ExpandErrorKind::ImportError, lst.sp, "rename expects module and pairs"));
    auto m = read_modname(lst.elems[1]); if (!m) return std::unexpected(m.error());
    ImportSet s; s.kind = ImportSet::Kind::Rename; s.module_name = *m; s.span = lst.sp;
    for (size_t i=2;i<lst.elems.size();++i) {
      if (!is_list(lst.elems[i])) return std::unexpected(err(ExpandErrorKind::ImportError, lst.elems[i]->span(), "rename pair must be list"));
      const auto& pr = as_list(lst.elems[i]);
      if (pr.elems.size()!=2 || !is_symbol(pr.elems[0]) || !is_symbol(pr.elems[1]))
        return std::unexpected(err(ExpandErrorKind::ImportError, pr.span, "rename pair must be (old new)"));
      s.renames.emplace_back(as_symbol(pr.elems[0]).name, as_symbol(pr.elems[1]).name);
    }
    return s;
  }

  return std::unexpected(err(ExpandErrorKind::ImportError, lst.sp, "unknown import clause"));
}

XResult<SExprPtr> Expander::handle_import(const List& lst) {
  if (!current_module_) return std::unexpected(err(ExpandErrorKind::ImportError, lst.sp, "import only valid inside a module"));
  if (lst.elems.size() < 2) return std::unexpected(err(ExpandErrorKind::ImportError, lst.sp, "import expects at least one clause"));

  for (size_t i=1;i<lst.elems.size();++i) {
    if (!is_list(lst.elems[i]) && !is_symbol(lst.elems[i]))
      return std::unexpected(err(ExpandErrorKind::ImportError, lst.elems[i]->span(), "bad import clause"));

    // Normalize to a list form for parser
    SExprPtr clause = lst.elems[i];
    ImportSet spec;
    if (is_symbol(clause)) {
      // bare: (import M) → treat as (M)
      auto tmp = list({ clause }, clause->span());
      auto specR = parse_import_set(as_list(tmp)); if (!specR) return std::unexpected(specR.error());
      spec = *specR;
    } else {
      auto specR = parse_import_set(as_list(clause)); if (!specR) return std::unexpected(specR.error());
      spec = *specR;
    }

    // Record pending import for link phase
    cu_.pending_imports[current_module_->name].push_back({ current_module_->name, spec, lst.sp });
  }

  // Directive no-op
  return list({ sym("begin", lst.sp) }, lst.sp);
}

// ---- Convenience sugar ----
XResult<SExprPtr> Expander::handle_defun(const List& lst) {
  if (lst.elems.size() < 3 || !is_symbol(lst.elems[1]) || !is_list(lst.elems[2]))
    return std::unexpected(err(ExpandErrorKind::InvalidSyntax, lst.sp, "defun expects (defun name (args) body...)"));
  auto name = lst.elems[1];
  std::vector<SExprPtr> lam; lam.push_back(sym("lambda", lst.sp));
  lam.push_back(lst.elems[2]);
  for (size_t i=3;i<lst.elems.size();++i) lam.push_back(lst.elems[i]);
  auto lamE = list(std::move(lam), lst.sp);
  auto lamX = handle_lambda(as_list(lamE)); if (!lamX) return std::unexpected(lamX.error());
  return list({ sym("define", lst.sp), name, *lamX }, lst.sp);
}

XResult<SExprPtr> Expander::handle_progn(const List& lst) {
  std::vector<SExprPtr> xs; xs.reserve(lst.elems.size());
  xs.push_back(sym("begin", lst.sp));
  for (size_t i=1;i<lst.elems.size();++i) { auto r = expand_form(lst.elems[i]); if (!r) return std::unexpected(r.error()); xs.push_back(*r);} 
  return list(std::move(xs), lst.sp);
}

// ---- Gensym ----
std::string Expander::gensym(const std::string& hint) {
  static uint64_t counter = 0;
  std::ostringstream oss; oss << hint << "__" << (++counter);
  return oss.str();
}

// ---- Link phase ----
XResult<void> Expander::link_all_modules() {
  // Validate exports exist in their module tables (optional but recommended)
  for (auto& [name, mod] : cu_.modules) {
    for (auto& ex : mod.exports) {
      if (mod.table.find(ex) == mod.table.end())
        return std::unexpected(err(ExpandErrorKind::ExportError, Span{}, "module '"+name+"' exports unknown identifier: "+ex));
    }
  }

  for (auto& [modName, imports] : cu_.pending_imports) {
    auto itM = cu_.modules.find(modName);
    if (itM == cu_.modules.end())
      return std::unexpected(err(ExpandErrorKind::ImportError, Span{}, "unknown module during link: "+modName));
    ModuleEnv& M = itM->second;

    for (auto& pi : imports) {
      auto itN = cu_.modules.find(pi.spec.module_name);
      if (itN == cu_.modules.end())
        return std::unexpected(err(ExpandErrorKind::ImportError, pi.where, "unknown module: "+pi.spec.module_name));
      ModuleEnv& N = itN->second;

      // Start set = N.exports
      std::unordered_map<std::string,std::string> map; // local -> remote
      for (const auto& ex : N.exports) map.emplace(ex, ex);

      // Apply filters
      switch (pi.spec.kind) {
        case ImportSet::Kind::Only: {
          std::unordered_map<std::string,std::string> f;
          for (auto& id : pi.spec.idents) {
            auto it = map.find(id);
            if (it == map.end())
              return std::unexpected(err(ExpandErrorKind::ImportError, pi.where, "module '"+N.name+"' does not export: "+id));
            f.emplace(id, id);
          }
          map.swap(f);
          break;
        }
        case ImportSet::Kind::Except: {
          for (auto& id : pi.spec.idents) map.erase(id);
          break;
        }
        case ImportSet::Kind::Rename: {
          for (auto& [oldn, newn] : pi.spec.renames) {
            auto it = map.find(oldn);
            if (it == map.end())
              return std::unexpected(err(ExpandErrorKind::ImportError, pi.where, "module '"+N.name+"' does not export: "+oldn));
            map.erase(it);
            map.emplace(newn, oldn);
          }
          break;
        }
        case ImportSet::Kind::Plain: default: break;
      }

      // Install into module M table; conflict if already present
      for (auto& [local, remote] : map) {
        if (is_reserved_keyword(local))
          return std::unexpected(err(ExpandErrorKind::ImportError, pi.where, "cannot import over reserved keyword: "+local));
        if (M.table.find(local) != M.table.end())
          return std::unexpected(err(ExpandErrorKind::ImportError, pi.where, "conflicting import or redefinition: "+local));
        M.table.emplace(local, Binding{ local, pi.where });
      }
    }
  }

  return {};
}

} // namespace eta::expand
```


### emitter.h (sketch, constant pool and symbol interning)
```cpp
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Keep emitter separate from runtime types. The VM loader will convert constants to runtime values.

struct ConstantValue {
  enum class Kind { Number, String, Symbol, QuotedData };
  Kind kind{};
  double num{}; // if Number
  std::string str; // String or Symbol text
  // For QuotedData, you can store a serialized SExpr or a handle to a constant blob

  static ConstantValue make_symbol(const std::string& s) { ConstantValue c; c.kind=Kind::Symbol; c.str=s; return c; }
  static ConstantValue make_string(const std::string& s) { ConstantValue c; c.kind=Kind::String; c.str=s; return c; }
  static ConstantValue make_number(double d) { ConstantValue c; c.kind=Kind::Number; c.num=d; return c; }
};

struct BytecodeEmitter {
  std::vector<ConstantValue> constant_pool;
  std::unordered_map<std::string, uint32_t> sym_intern; // string -> const index
  std::vector<uint8_t> code;

  void emit_op(uint8_t op) { code.push_back(op); }
  void emit_u32(uint32_t v) {
    code.push_back((uint8_t)(v & 0xFF));
    code.push_back((uint8_t)((v>>8) & 0xFF));
    code.push_back((uint8_t)((v>>16) & 0xFF));
    code.push_back((uint8_t)((v>>24) & 0xFF));
  }

  uint32_t intern_symbol(const std::string& s) {
    auto it = sym_intern.find(s);
    if (it != sym_intern.end()) return it->second;
    uint32_t idx = (uint32_t)constant_pool.size();
    constant_pool.push_back(ConstantValue::make_symbol(s));
    sym_intern.emplace(s, idx);
    return idx;
  }

  void emit_symbol_ref(const std::string& sym) {
    uint32_t idx = intern_symbol(sym);
    emit_op(/*OP_LOAD_CONST*/ 0x01);
    emit_u32(idx);
  }
};
```


### Tests (ETA/Scheme snippets)
Use these in your test harness to validate behavior.

```scheme
; lambda params
((lambda (x y . rest) (list x y rest)) 1 2 3 4)   ; => (1 2 (3 4))
((lambda args args) 1 2 3)                        ; => (1 2 3)

; let family
(let ((x 1) (y 2)) (+ x y))                        ; => 3
(let* ((x 1) (y (+ x 2))) y)                       ; => 3
(letrec ((even? (lambda (n) (if (= n 0) #t (odd? (- n 1)))))
         (odd?  (lambda (n) (if (= n 0) #f (even? (- n 1))))))
  (even? 10))                                      ; => #t

; named let
(let loop ((i 3) (acc 1))
  (if (= i 0) acc (loop (- i 1) (* acc i))))       ; => 6

; cond with =>
(cond ((string? s) => string-length)
      ((number? s) 0)
      (else -1))

; modules
(module m
  (define x 10)
  (define (inc n) (+ n 1))
  (export x inc))

(module u
  (import (only (m) inc))
  (define y (inc 1))
  (export y))

; circular imports (link phase handles this)
(module A
  (export a)
  (import (B))
  (define a 1))

(module B
  (export b)
  (import (A))
  (define b a))

; conflicts and errors (should raise during expansion/linking)
(lambda (x x) x)                                   ; duplicate parameter -> error
(export x)                                         ; outside module -> error
(import (nope))                                    ; unknown module -> error at link or import if bare
(lambda (x . y z) x)                               ; malformed dotted form -> error
(module M (export x) (begin))                      ; export of unknown id -> link-time error
(module U (define x 0) (import (M)))               ; conflicting import -> link-time error
```


### Integration checklist
- Replace factory/predicate stubs with your actual `SExpr`/`List`/`Symbol` constructors and matchers. Ensure `is_dot` returns true for dot token or make `is_core_dot` check symbol(".").
- Wire the expander into your compilation driver:
  - Read forms → `std::vector<SExprPtr>`
  - Call `Expander::expand_and_link(forms)`
  - Feed the expanded forms to your later phases (e.g., analyzer/binder or directly to the emitter).
- Ensure `handle_define` updates the current module’s `table`. This enables forward references and export validation.
- Keep `handle_import` as a directive: record `ImportSet` into `cu_.pending_imports` and do not modify the env at expansion time.
- After processing all forms: call `link_all_modules()` (already done by `expand_and_link`).
- Reserved keywords: extend `reserved_keywords()` set if you add more core forms. These checks are applied in `define`, `lambda` params, `let` bindings, and import name installation.
- If you have a dedicated `AST::ModuleForm`, construct and return it in `handle_module` instead of the placeholder `(ModuleForm name (begin ...))` list.
- Quasiquote support: keep your existing implementation; this expander dispatch doesn’t interfere with it.
- Spans: propagate and merge spans appropriately in your list constructors for accurate diagnostics.
- Emitter: keep constant pool and symbol interning distinct from runtime. The VM loader converts pool entries to runtime values via your `runtime/factory.h`.


### Rationale and guarantees
- Two-phase module system eliminates deadlocks and supports circular imports by separating export discovery from import linking.
- Lexical env remains for scopes inside forms; module registry/table is kept in `CompilationUnit`, clarifying cross-module vs. lexical concerns.
- Desugarings follow Scheme semantics and keep bodies expanded in correct environments.
- Duplicate/keyword protections reduce subtle bugs and align with Scheme conventions.

This is the full solution you can adapt into your repository. If you supply your exact `SExpr` node APIs and constructor names, I can translate these sketches into code that compiles verbatim in your project.```}