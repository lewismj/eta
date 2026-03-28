Overview
This plan adds a two‑phase Module Linker that operates after the expander, resolving cross‑module names and export/import filters while supporting circular dependencies. It aligns with the current codebase patterns (std::expected for error transport, parser::SExpr for syntax trees, parser::Span for diagnostics) and the expander’s normalization of module forms to concrete lists: (module name (export ...) ...expanded body...).

The Module Linker is self‑contained in module_linker.h/.cpp and exposes three main APIs: index_modules, link, and get. The first phase scans modules to collect defined, exports, and pending imports. The second phase resolves all imports and produces per‑module visible sets. Errors carry precise Spans from import/export sites.

Files and namespaces
Add files:
core\src\eta\reader\module_linker.h
core\src\eta\reader\module_linker.cpp
Namespace: eta::reader::linker
Includes needed:
#include <expected>
#include <string> / <vector> / <unordered_map> / <unordered_set> / <utility>
#include "eta/reader/parser.h" (for parser::SExpr, parser::SExprPtr, parser::List, parser::Symbol, parser::Span)
Public API (header)
#pragma once

#include <expected>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "eta/reader/parser.h"

namespace eta::reader::linker {

using eta::reader::parser::SExpr;      
using eta::reader::parser::SExprPtr;   
using eta::reader::parser::List;       
using eta::reader::parser::Symbol;     
using eta::reader::parser::Span;       

struct LinkError {
    enum class Kind : std::uint8_t {
        UnknownModule,          // import source missing
        ExportOfUnknownName,    // (export x) but x not defined in that module
        ConflictingImport,      // local name clashes with local define or prior import
        NameNotExported,        // only/rename names not exported by source
        DuplicateModule         // two modules with the same name
    } kind{};
    Span span{};
    std::string message;
};

template <typename T>
using LinkResult = std::expected<T, LinkError>;

struct ModuleTable {
    std::string name;
    std::unordered_set<std::string> defined;             // names defined in this module
    std::unordered_set<std::string> exports;             // names exported (declared)
    std::unordered_set<std::string> visible;             // locals + imported aliases (after link)

    // For diagnostics
    std::unordered_map<std::string, Span> define_spans;  // name -> span of its define
    std::unordered_map<std::string, Span> export_spans;  // name -> span where exported
};

struct ImportSpec {
    enum class Kind { Plain, Only, Except, Rename } kind{Kind::Plain};
    std::string module;  
    std::vector<std::string> ids; // for Only/Except
    std::vector<std::pair<std::string,std::string>> renames; // (old,new) for Rename
    Span span{};                     // span of the clause (or symbol for Plain)
};

struct PendingImport {
    std::string target;   // target module name (receiver)
    ImportSpec spec;      // import clause
    Span where;           // site of the (import ...) form or clause
};

class ModuleLinker {
public:
    // Input: top-level forms post-expander (many module lists possible)
    LinkResult<void> index_modules(const std::vector<SExprPtr>& forms);

    // Resolve pending imports into ModuleTable.visible
    LinkResult<void> link();

    const ModuleTable* get(const std::string& name) const;

private:
    std::unordered_map<std::string, ModuleTable> modules_; // name -> table
    std::unordered_map<std::string, std::vector<PendingImport>> pending_; // target -> imports

    // Helpers
    static bool is_symbol_named(const SExprPtr& p, std::string_view name);
    static const List* as_list(const SExprPtr& p) { return (p && p->is<List>()) ? p->as<List>() : nullptr; }
    static const Symbol* as_symbol(const SExprPtr& p) { return (p && p->is<Symbol>()) ? p->as<Symbol>() : nullptr; }

    LinkResult<void> index_one_module_list(const List& module_form);
    LinkResult<void> scan_module_body(ModuleTable& mt, const std::vector<SExprPtr>& body);
    LinkResult<void> parse_import_form(ModuleTable& mt, const List& import_form);
    LinkResult<ImportSpec> parse_import_clause(const SExprPtr& clause);
    static std::string to_string(const LinkError::Kind k);
};

} // namespace eta::reader::linker
Implementation details (cpp)
1) Utilities
is_symbol_named(p, name): identical semantics to the expander’s helper (local small duplicate to avoid coupling).
as_list/as_symbol: safe casts using SExpr methods already present.
#include "eta/linker/module_linker.h"
#include <sstream>

namespace eta::reader::linker {

bool ModuleLinker::is_symbol_named(const SExprPtr& p, std::string_view name) {
    auto s = as_symbol(p); return s && s->name == name;
}

const ModuleTable* ModuleLinker::get(const std::string& name) const {
    auto it = modules_.find(name); return it == modules_.end() ? nullptr : &it->second;
}

std::string ModuleLinker::to_string(const LinkError::Kind k) {
    using K = LinkError::Kind;
    switch (k) {
        case K::UnknownModule: return "UnknownModule";
        case K::ExportOfUnknownName: return "ExportOfUnknownName";
        case K::ConflictingImport: return "ConflictingImport";
        case K::NameNotExported: return "NameNotExported";
        case K::DuplicateModule: return "DuplicateModule";
    }
    return "LinkerError";
}
2) Phase 1: Index
Walk post‑expander top‑level forms.
Recognize module forms: proper list whose head is symbol module, followed by symbol module name, followed by zero or more body forms.
Enforce unique module names.
Scan body for define, export, and import only (the expander guarantees this shape if input came from ModuleForm or handle_module_list).
LinkResult<void> ModuleLinker::index_modules(const std::vector<SExprPtr>& forms) {
    modules_.clear(); pending_.clear();

    // Pass 1: collect modules and their bodies
    for (const auto& f : forms) {
        const auto* lst = as_list(f); if (!lst) continue; // ignore atoms
        if (lst->elems.size() < 2) continue;
        if (!is_symbol_named(lst->elems[0], "module")) continue;
        auto nameSym = as_symbol(lst->elems[1]);
        if (!nameSym) {
            return std::unexpected(LinkError{LinkError::Kind::DuplicateModule, lst->span,
                "module: invalid module name (symbol required)"});
        }
        auto name = nameSym->name;
        if (modules_.contains(name)) {
            return std::unexpected(LinkError{LinkError::Kind::DuplicateModule, nameSym->span,
                "duplicate module: " + name});
        }

        ModuleTable mt; mt.name = name;
        // Extract the tail of the module list as body forms
        std::vector<SExprPtr> body;
        body.reserve(lst->elems.size() - 2);
        for (size_t i = 2; i < lst->elems.size(); ++i) body.push_back(eta::reader::parser::SExprPtr{lst->elems[i] ? lst->elems[i]->as<SExpr>() : nullptr});
        // Note: we won't move; we will rescan via original pointers below to keep ownership with caller.

        // Scan definitions/exports/imports directly from lst
        auto res = scan_module_body(mt, {lst->elems.begin() + 2, lst->elems.end()});
        if (!res) return res;
        modules_.emplace(name, std::move(mt));
    }

    // Validate exports: must refer to local defines
    for (auto& [name, mt] : modules_) {
        for (const auto& [ex, sp] : mt.export_spans) {
            if (!mt.defined.contains(ex)) {
                return std::unexpected(LinkError{LinkError::Kind::ExportOfUnknownName, sp,
                    "export of unknown name '" + ex + "' in module " + name});
            }
        }
    }

    return {};
}
Notes:

We avoid deep copies; we only read shapes.
We keep export_spans for accurate diagnostics.
3) Scanning module bodies
define scanning: after the expander, function defines are normalized to (define id expr). So scan (define <symbol> ...) and record into defined/define_spans.
export scanning: (export id...) each id as symbol; record into exports and export_spans.
import scanning: (import clause...), where each clause is:
Symbol → ImportSpec{Kind::Plain, module=<symbol>}
(only M a b ...)
(except M a b ...)
(rename M (a x) (b y) ...)
LinkResult<void> ModuleLinker::scan_module_body(ModuleTable& mt, const std::vector<SExprPtr>& body) {
    for (const auto& f : body) {
        const auto* l = as_list(f); if (!l) continue; // ignore atoms
        if (l->elems.empty() || !l->elems[0]) continue;
        const auto* head = as_symbol(l->elems[0]); if (!head) continue;
        const auto& kw = head->name;

        if (kw == "define") {
            if (l->elems.size() >= 3 && as_symbol(l->elems[1])) {
                const auto* s = as_symbol(l->elems[1]);
                mt.defined.insert(s->name);
                mt.define_spans.emplace(s->name, s->span);
            }
            continue;
        }
        if (kw == "export") {
            for (size_t i = 1; i < l->elems.size(); ++i) {
                if (auto s = as_symbol(l->elems[i])) {
                    mt.exports.insert(s->name);
                    mt.export_spans.emplace(s->name, s->span);
                }
            }
            continue;
        }
        if (kw == "import") {
            auto r = parse_import_form(mt, *l); if (!r) return r; continue;
        }
    }
    return {};
}

LinkResult<void> ModuleLinker::parse_import_form(ModuleTable& mt, const List& import_form) {
    for (size_t i = 1; i < import_form.elems.size(); ++i) {
        const auto& c = import_form.elems[i]; if (!c) continue;
        auto spec = parse_import_clause(c);
        if (!spec) return std::unexpected(spec.error());
        pending_[mt.name].push_back(PendingImport{mt.name, *spec, spec->span});
    }
    return {};
}

LinkResult<ImportSpec> ModuleLinker::parse_import_clause(const SExprPtr& clause) {
    if (auto s = as_symbol(clause)) {
        ImportSpec sp; sp.kind = ImportSpec::Kind::Plain; sp.module = s->name; sp.span = s->span; return sp;
    }
    const auto* l = as_list(clause);
    if (!l || l->elems.empty() || !as_symbol(l->elems[0])) {
        return std::unexpected(LinkError{LinkError::Kind::UnknownModule, clause ? clause->span() : Span{},
            "import: invalid clause"});
    }
    const auto* head = as_symbol(l->elems[0]);
    auto bad = [&](std::string msg) -> LinkResult<ImportSpec> { return std::unexpected(LinkError{LinkError::Kind::UnknownModule, l->span, std::move(msg)}); };

    auto req_mod = [&](size_t idx) -> const Symbol* { return (l->elems.size() > idx) ? as_symbol(l->elems[idx]) : nullptr; };

    if (head->name == "only") {
        const auto* m = req_mod(1); if (!m) return bad("(only m ids...) requires module symbol");
        ImportSpec sp; sp.kind = ImportSpec::Kind::Only; sp.module = m->name; sp.span = l->span;
        for (size_t i = 2; i < l->elems.size(); ++i) { if (auto s = as_symbol(l->elems[i])) sp.ids.push_back(s->name); }
        return sp;
    }
    if (head->name == "except") {
        const auto* m = req_mod(1); if (!m) return bad("(except m ids...) requires module symbol");
        ImportSpec sp; sp.kind = ImportSpec::Kind::Except; sp.module = m->name; sp.span = l->span;
        for (size_t i = 2; i < l->elems.size(); ++i) { if (auto s = as_symbol(l->elems[i])) sp.ids.push_back(s->name); }
        return sp;
    }
    if (head->name == "rename") {
        const auto* m = req_mod(1); if (!m) return bad("(rename m (old new) ...) requires module symbol");
        ImportSpec sp; sp.kind = ImportSpec::Kind::Rename; sp.module = m->name; sp.span = l->span;
        for (size_t i = 2; i < l->elems.size(); ++i) {
            const auto* pair = as_list(l->elems[i]); if (!pair || pair->elems.size() != 2) continue;
            const auto* oldS = as_symbol(pair->elems[0]); const auto* newS = as_symbol(pair->elems[1]);
            if (oldS && newS) sp.renames.emplace_back(oldS->name, newS->name);
        }
        return sp;
    }

    // Fallback: treat as plain module symbol embedded in list (not typical)
    if (l->elems.size() == 1 && as_symbol(l->elems[0])) {
        ImportSpec sp; sp.kind = ImportSpec::Kind::Plain; sp.module = as_symbol(l->elems[0])->name; sp.span = l->span; return sp;
    }
    return std::unexpected(LinkError{LinkError::Kind::UnknownModule, l->span, "import: unknown clause kind"});
}
4) Phase 2: Link
For each target module, process its pending_ imports.
For each PendingImport:
Locate source module; if not found → UnknownModule error.
Start map: local -> remote from the source’s exports as local=remote=name.
Apply filters:
Only: keep only listed; if any listed not exported → NameNotExported.
Except: erase those present; unknown names are ignored.
Rename: for each pair (old,new), ensure old is exported; then set map[new]=old (and remove old if present). Collisions among new → ConflictingImport.
For each (local, remote) in map: if local conflicts with target defined or is already imported → ConflictingImport (report both names if possible). Otherwise insert local into visible.
After processing all imports of a module, set visible ∪= defined.
LinkResult<void> ModuleLinker::link() {
    // Prepare per-target import tracking for conflict diagnostics
    struct ImportOrigin { std::string from_module; Span where; };
    std::unordered_map<std::string, std::unordered_map<std::string, ImportOrigin>> imported_by_module; // target -> (local -> origin)

    for (auto& [target_name, imports] : pending_) {
        auto tIt = modules_.find(target_name); if (tIt == modules_.end()) continue; // defensive
        ModuleTable& tgt = tIt->second;

        auto& imported = imported_by_module[target_name];

        for (const auto& pi : imports) {
            auto sIt = modules_.find(pi.spec.module);
            if (sIt == modules_.end()) {
                return std::unexpected(LinkError{LinkError::Kind::UnknownModule, pi.where,
                    "unknown module in import: " + pi.spec.module});
            }
            const ModuleTable& src = sIt->second;

            // Build map: local -> remote
            std::unordered_map<std::string, std::string> map; map.reserve(src.exports.size());
            for (const auto& ex : src.exports) map.emplace(ex, ex);

            auto ensure_exported = [&](const std::string& name, const char* ctx) -> LinkResult<void> {
                if (!src.exports.contains(name)) {
                    std::ostringstream oss; oss << "import " << ctx << ": name '" << name << "' is not exported by module '" << src.name << "'";
                    return std::unexpected(LinkError{LinkError::Kind::NameNotExported, pi.where, oss.str()});
                }
                return {};
            };

            switch (pi.spec.kind) {
                case ImportSpec::Kind::Plain:
                    // nothing
                    break;
                case ImportSpec::Kind::Only: {
                    std::unordered_map<std::string, std::string> filtered; filtered.reserve(pi.spec.ids.size());
                    for (const auto& id : pi.spec.ids) {
                        if (auto ok = ensure_exported(id, "only"); !ok) return ok;
                        filtered.emplace(id, id);
                    }
                    map.swap(filtered);
                    break;
                }
                case ImportSpec::Kind::Except: {
                    for (const auto& id : pi.spec.ids) { map.erase(id); }
                    break;
                }
                case ImportSpec::Kind::Rename: {
                    for (const auto& [oldn, newn] : pi.spec.renames) {
                        if (auto ok = ensure_exported(oldn, "rename"); !ok) return ok;
                        // If not already present in map (because of previous filters), add
                        if (!map.contains(oldn)) map.emplace(oldn, oldn);
                    }
                    // Apply renames
                    for (const auto& [oldn, newn] : pi.spec.renames) {
                        auto it = map.find(oldn); if (it == map.end()) continue; // filtered out by Only
                        map.erase(it);
                        if (map.contains(newn)) {
                            return std::unexpected(LinkError{LinkError::Kind::ConflictingImport, pi.where,
                                "rename produces duplicate local name '" + newn + "'"});
                        }
                        map.emplace(newn, oldn);
                    }
                    break;
                }
            }

            // Commit to target.visible with conflict checks
            for (const auto& [local, remote] : map) {
                if (tgt.defined.contains(local)) {
                    return std::unexpected(LinkError{LinkError::Kind::ConflictingImport, pi.where,
                        "imported name '" + local + "' conflicts with local define in module '" + tgt.name + "'"});
                }
                if (imported.contains(local)) {
                    const auto& prev = imported.at(local);
                    std::ostringstream oss; oss << "conflicting imports for '" << local << "' from '" << prev.from_module << "' and '" << src.name << "'";
                    return std::unexpected(LinkError{LinkError::Kind::ConflictingImport, pi.where, oss.str()});
                }
                imported.emplace(local, ImportOrigin{src.name, pi.where});
                tgt.visible.insert(local);
            }
        }

        // Make locals visible too
        for (const auto& d : tgt.defined) tgt.visible.insert(d);
    }
    return {};
}

} // namespace eta::reader::linker
Implementation notes:

We track per‑target imported locals to detect import‑vs‑import name clashes.
We only require the exports set from sources during link, so modules can be linked in any order (circular import safe).
Behavioral details and edge cases
Module recognition: we only act on lists headed by module. If non‑module top‑level forms exist, they’re ignored by the linker (can later be wrapped into a synthetic module if desired).
Duplicate module names: reported at indexing.
Export validation: an (export x) where x wasn’t defined yields an ExportOfUnknownName error at the export site.
Import filters:
Plain: imports all exports of the source as local names.
Only: must reference names exported by the source, else NameNotExported.
Except: erases if present; unknown names are ignored.
Rename: (rename M (a x) (b y) ...) requires each a to be exported by M and applies renames; collisions among new produce ConflictingImport.
Conflicts:
Imported local vs local define → ConflictingImport.
Imported local vs previously imported local → ConflictingImport.
Circular imports: allowed because we never dereference visible from sources during link; only exports are needed (available after indexing).
Tests to add (Boost.Test)
Create test\src\module_linker_tests.cpp with the following patterns. Tests should construct source text, run the existing reader+expander, and feed the expanded forms into ModuleLinker.

Helpers (pseudo):

static std::vector<parser::SExprPtr> parse_and_expand(std::string_view program) {
    using namespace eta::reader;
    lexer::Lexer lex({program.data(), program.size()});
    parser::Parser p(lex);
    auto parsed = p.parse_toplevel();
    BOOST_REQUIRE(parsed.has_value());

    expander::Expander ex;
    auto expanded = ex.expand_many(*parsed);
    BOOST_REQUIRE(expanded.has_value());
    return std::move(*expanded);
}
Plain import:
(module m1 (define a 1) (define b 2) (export a b))
(module m2 (import m1) (define x a))
Expect: m2.visible contains a, b, x.

Only:
(module m1 (define a 1) (define b 2) (define c 3) (export a b c))
(module m2 (import (only m1 a c)) (define y (+ a c)))
Expect: m2.visible contains a, c, y but not b.

Except:
(module m1 (define a 1) (define b 2) (define c 3) (export a b c))
(module m2 (import (except m1 b)) (define y (+ a c)))
Expect: m2.visible contains a, c, y but not b.

Rename:
(module m1 (define a 1) (define c 3) (export a c))
(module m2 (import (rename m1 (a x) (c z))) (define y (+ x z)))
Expect: m2.visible contains x, z, y but not a, c.

Conflicting import vs local define:
(module m1 (define a 1) (export a))
(module m2 (define a 2) (import m1))
Expect: link() returns ConflictingImport at the import site.

Export of missing name:
(module m1 (export x))
Expect: index_modules() returns ExportOfUnknownName at the export site.

Circular imports A↔B:
(module A (export a) (import B) (define a 1))
(module B (export b) (import A) (define b a))
Expect: both link successfully, A.visible contains a and b? No: A imports B with Plain, so A.visible contains a (local) and b (from B); likewise B.visible contains b and a.

Skeleton test checks:

BOOST_AUTO_TEST_CASE(linker_plain_import) {
    auto forms = parse_and_expand("(module m1 (define a 1) (define b 2) (export a b))\n(module m2 (import m1) (define x a))");
    eta::linker::ModuleLinker L;
    auto idx = L.index_modules(forms); BOOST_REQUIRE(idx.has_value());
    auto lk = L.link(); BOOST_REQUIRE(lk.has_value());
    auto m2 = L.get("m2"); BOOST_REQUIRE(m2);
    BOOST_CHECK(m2->visible.contains("a"));
    BOOST_CHECK(m2->visible.contains("b"));
    BOOST_CHECK(m2->visible.contains("x"));
}
Integration points
The new pass sits after the expander and before the (future) semantic analyzer.
Provide a small façade or pipeline function later that:
Reader → parser::Parser::parse_toplevel()
Expander → expander::Expander::expand_many()
Linker → ModuleLinker::index_modules() then ModuleLinker::link()
Complexity
Indexing: O(total body forms) across modules.
Linking: O(sum over imports of size(exports(source))) in the worst case, with hash lookups O(1) average.
Step-by-step implementation checklist
Create module_linker.h/.cpp with the API and types above.
Implement helpers (is_symbol_named, as_list, as_symbol).
Implement index_modules: walk forms, find module lists, uniqueness check, call scan_module_body and store tables; validate exports.
Implement scan_module_body: gather defined/exports and queue PendingImports via parse_import_form and parse_import_clause.
Implement link: resolve pending_ using only exports from sources; detect conflicts; populate visible.
Add tests in test/src/module_linker_tests.cpp for: plain import, only, except, rename, conflicts, export missing, circular A↔B, and a few mixed multi‑clause imports.
Optionally, add to_string(LinkError::Kind) and stream printer for user‑friendly diagnostics.
Future extensions (nice-to-have)
Allow (import (prefix m p-)) style (not in current scope).
Track import provenance in ModuleTable for better error messages in the semantic analyzer.
Optionally introduce a State per module (Indexed, Linked) for debugging.
Why this fits the current codebase
Uses std::expected like the parser/expander (ExpanderResult style), with an explicit LinkError carrying Span and message.
Operates on post‑expander list shapes that the expander already guarantees (handle_module_list, handle_export, handle_import).
Avoids AST desugaring in the linker; it only reasons over module scopes and names.
Supports circular imports because linking relies solely on exports, available immediately after indexing.
This plan yields a focused, maintainable linker pass (~200–300 LOC) that slots cleanly between the expander and the forthcoming semantic analyzer.