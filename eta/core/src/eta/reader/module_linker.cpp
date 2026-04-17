#include <sstream>
#include "module_linker.h"

namespace eta::reader::linker {

    using namespace utils;


    std::optional<std::reference_wrapper<const ModuleTable>>
    ModuleLinker::get(const std::string& name) const {
        auto it = modules_.find(name);
        if (it == modules_.end()) return std::nullopt;
        return std::cref(it->second);
    }

    /// enum printer moved to free constexpr function in header

    /// --- Indexing ---

    LinkResult<void> ModuleLinker::index_modules(std::span<const SExprPtr> forms) {
        modules_.clear(); pending_.clear();

        for (const auto& f : forms) {
            const auto* lst = as_list(f); if (!lst) continue; ///< ignore atoms
            if (lst->elems.size() < 2) continue;
            if (!is_symbol_named(lst->elems[0], "module")) continue;
            auto nameSym = as_symbol(lst->elems[1]);
            if (!nameSym) {
                /// More appropriate than DuplicateModule: the form is not a valid module declaration
                return std::unexpected(LinkError{LinkError::Kind::UnknownModule, lst->span,
                    "module: invalid module name (symbol required)"});
            }
            auto name = nameSym->name;
            if (modules_.contains(name)) {
                return std::unexpected(LinkError{LinkError::Kind::DuplicateModule, nameSym->span,
                    std::string("duplicate module: ") + name});
            }

            ModuleTable mt; mt.name = name; mt.state = ModuleState::Indexed;
            auto res = scan_module_body(*lst, mt);
            if (!res) return res;
            modules_.emplace(name, std::move(mt));
        }

        /**
         * NOTE: Export validation is deferred to link() so that re-exports
         * of imported names (e.g. std.prelude re-exporting std.collections)
         * are checked against the fully-resolved visible set.
         */

        return {};
    }

    LinkResult<void> ModuleLinker::scan_module_body(const List& module_form, ModuleTable& mt) {
        /// Body starts at index 2
        for (size_t i = 2; i < module_form.elems.size(); ++i) {
            const auto& f = module_form.elems[i];
            const auto* l = as_list(f); if (!l) continue; ///< ignore atoms
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
                for (size_t j = 1; j < l->elems.size(); ++j) {
                    if (auto s = as_symbol(l->elems[j])) {
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
        if (head->name == "prefix") {
            if (l->elems.size() != 3) return bad("(prefix mod prefix-string) requires exactly 2 arguments");
            const auto* m = req_mod(1); if (!m) return bad("(prefix mod pfx) requires module symbol");
            const auto* pfx = as_symbol(l->elems[2]); if (!pfx) return bad("(prefix mod pfx) requires prefix symbol");
            ImportSpec sp; sp.kind = ImportSpec::Kind::Prefix; sp.module = m->name; sp.prefix = pfx->name; sp.span = l->span;
            return sp;
        }

        /// Fallback: treat as plain module symbol embedded in list (not typical)
        if (l->elems.size() == 1 && as_symbol(l->elems[0])) {
            ImportSpec sp; sp.kind = ImportSpec::Kind::Plain; sp.module = as_symbol(l->elems[0])->name; sp.span = l->span; return sp;
        }
        return std::unexpected(LinkError{LinkError::Kind::UnknownModule, l->span, "import: unknown clause kind"});
    }

    /// --- Linking ---
    namespace {
        struct CycleDetector {
            enum State { Unvisited, Visiting, Visited };
            const std::unordered_map<std::string, std::vector<PendingImport>>& pending;
            std::unordered_map<std::string, State> states;
            std::vector<std::string> path;

            LinkResult<void> visit(const std::string& name) {
                auto& state = states[name];
                if (state == Visiting) {
                    std::string cycle;
                    bool in_cycle = false;
                    for (const auto& p : path) {
                        if (p == name) in_cycle = true;
                        if (in_cycle) { cycle += p; cycle += " -> "; }
                    }
                    cycle += name;
                    return std::unexpected(LinkError{LinkError::Kind::CircularDependency, {},
                        "Circular module dependency detected: " + cycle});
                }
                if (state == Visited) return {};

                state = Visiting;
                path.push_back(name);
                if (auto it = pending.find(name); it != pending.end()) {
                    for (const auto& pi : it->second) {
                        if (auto res = visit(pi.spec.module); !res) return res;
                    }
                }
                path.pop_back();
                state = Visited;
                return {};
            }
        };
    }

    LinkResult<void> ModuleLinker::link() {
        CycleDetector detector{pending_, {}, {}};
        for (const auto& [name, _] : modules_) {
            if (auto res = detector.visit(name); !res) return res;
        }

        for (auto& [target_name, imports] : pending_) {
            auto tIt = modules_.find(target_name); if (tIt == modules_.end()) continue; ///< defensive
            ModuleTable& tgt = tIt->second;

            for (const auto& pi : imports) {
                auto sIt = modules_.find(pi.spec.module);
                if (sIt == modules_.end()) {
                    return std::unexpected(LinkError{LinkError::Kind::UnknownModule, pi.where,
                        std::string("unknown module in import: ") + pi.spec.module});
                }
                const ModuleTable& src = sIt->second;

                /// Build map: local -> remote (start from all exports for this clause)
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
                        /// keep full export set
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
                        /// Validate each old name is exported
                        for (const auto& [oldn, newn] : pi.spec.renames) {
                            (void)newn;
                            if (auto ok = ensure_exported(oldn, "rename"); !ok) return ok;
                        }
                        /// Apply renames strictly to the current map (do not re-add filtered-out names)
                        for (const auto& [oldn, newn] : pi.spec.renames) {
                            auto it = map.find(oldn);
                            if (it == map.end()) {
                                std::ostringstream oss; oss << "rename: name '" << oldn << "' is not in the current import set";
                                return std::unexpected(LinkError{LinkError::Kind::NameNotExported, pi.where, oss.str()});
                            }
                            auto remote = it->second; ///< remember remote source name
                            map.erase(it);
                            if (map.contains(newn)) {
                                return std::unexpected(LinkError{LinkError::Kind::ConflictingImport, pi.where,
                                    std::string("rename produces duplicate local name '") + newn + "'"});
                            }
                            map.emplace(newn, remote);
                        }
                        break;
                    }
                    case ImportSpec::Kind::Prefix: {
                        /// Prepend prefix to every local name; remote stays the same
                        std::unordered_map<std::string, std::string> prefixed;
                        prefixed.reserve(map.size());
                        for (const auto& [local, remote] : map) {
                            prefixed.emplace(pi.spec.prefix + local, remote);
                        }
                        map.swap(prefixed);
                        break;
                    }
                }

                /// Commit to target.visible with conflict checks; record provenance
                for (const auto& [local, remote] : map) {
                    if (tgt.defined.contains(local)) {
                        return std::unexpected(LinkError{LinkError::Kind::ConflictingImport, pi.where,
                            std::string("imported name '") + local + "' conflicts with local define in module '" + tgt.name + "'"});
                    }
                    if (tgt.import_origins.contains(local)) {
                        const auto& prev = tgt.import_origins.at(local);

                        /// If this is the same binding (same module + same remote name), treat as idempotent.
                        if (prev.from_module == src.name && prev.remote_name == remote) {
                            continue;
                        }

                        std::ostringstream oss; oss << "conflicting imports for '" << local << "' from '" << prev.from_module << "' and '" << src.name << "'";
                        return std::unexpected(LinkError{LinkError::Kind::ConflictingImport, pi.where, oss.str()});
                    }
                    tgt.visible.insert(local);
                    tgt.import_origins.emplace(local, ImportOrigin{src.name, remote, pi.where});
                }
            }

            /// Make locals visible too (after imports, to allow conflict checks above)
            for (const auto& d : tgt.defined) tgt.visible.insert(d);
            tgt.state = ModuleState::Linked;
        }

        /// Ensure all modules (even those without imports) get linked
        for (auto& [name, mt] : modules_) {
            if (mt.state == ModuleState::Indexed) {
                for (const auto& d : mt.defined) mt.visible.insert(d);
                mt.state = ModuleState::Linked;
            }
        }

        /// Validate exports: every exported name must be visible (locally defined OR imported)
        for (const auto& [name, mt] : modules_) {
            for (const auto& [ex, sp] : mt.export_spans) {
                if (!mt.visible.contains(ex)) {
                    return std::unexpected(LinkError{LinkError::Kind::ExportOfUnknownName, sp,
                        std::string("export of unknown name '") + ex + "' in module " + name});
                }
            }
        }

        return {};
    }

} ///< namespace eta::linker
