/**
 * @file compilation_session.cpp
 * @brief Incremental compilation/link state for eta session runtimes.
 */

#include "eta/session/compilation_session.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

#include "eta/diagnostic/diagnostic.h"
#include "eta/reader/expander.h"
#include "eta/reader/lexer.h"
#include "eta/reader/module_linker.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/extension_env.h"
#include "eta/runtime/vm/bytecode_serializer.h"
#include "eta/runtime/vm/vm.h"
#include "eta/semantics/emitter.h"
#include "eta/semantics/optimization_pipeline.h"
#include "eta/semantics/semantic_analyzer.h"
#include "eta/session/runtime_primitives.h"
#include "eta/util/path.h"

namespace eta::session {

bool CompilationSession::has_module(const std::string& name) const noexcept {
    return executed_modules_.contains(name);
}

bool CompilationSession::can_register_extension_primitives() const noexcept {
    return accumulated_forms_.empty()
        && executed_modules_.empty()
        && runtime_module_info_.empty();
}

const std::optional<std::filesystem::path>&
CompilationSession::prelude_origin_path() const noexcept {
    return prelude_origin_path_;
}

void CompilationSession::set_prelude_origin_path(
    std::optional<std::filesystem::path> path) {
    prelude_origin_path_ = std::move(path);
}

const std::unordered_map<uint32_t, std::string>&
CompilationSession::global_names() const noexcept {
    return global_names_;
}

std::unordered_map<uint32_t, std::string>&
CompilationSession::mutable_global_names() noexcept {
    return global_names_;
}

const std::unordered_map<std::string, CompilationSession::CompiledModuleLinkInfo>&
CompilationSession::compiled_link_modules() const noexcept {
    return compiled_link_modules_;
}

const CompilationSession::RuntimeModuleInfo* CompilationSession::runtime_module_info(
    const std::string& module_name) const noexcept {
    const auto it = runtime_module_info_.find(module_name);
    if (it == runtime_module_info_.end()) return nullptr;
    return &it->second;
}

std::optional<uint32_t> CompilationSession::runtime_export_slot(
    std::string_view module_name,
    std::string_view export_name) const {
    const auto module_it = runtime_module_info_.find(std::string(module_name));
    if (module_it == runtime_module_info_.end()) return std::nullopt;
    const auto export_it =
        module_it->second.export_slots.find(std::string(export_name));
    if (export_it == module_it->second.export_slots.end()) return std::nullopt;
    return export_it->second;
}

void CompilationSession::mark_module_executed(std::string module_name) {
    executed_modules_.insert(std::move(module_name));
}

CompilationSession::ActiveModuleExecutionGuard::ActiveModuleExecutionGuard(
    ActiveModuleExecutionGuard&& other) noexcept
    : session_(other.session_),
      inserted_modules_(std::move(other.inserted_modules_)) {
    other.session_ = nullptr;
}

CompilationSession::ActiveModuleExecutionGuard&
CompilationSession::ActiveModuleExecutionGuard::operator=(
    ActiveModuleExecutionGuard&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    reset();
    session_ = other.session_;
    inserted_modules_ = std::move(other.inserted_modules_);
    other.session_ = nullptr;
    return *this;
}

CompilationSession::ActiveModuleExecutionGuard::~ActiveModuleExecutionGuard() {
    reset();
}

void CompilationSession::ActiveModuleExecutionGuard::reset() noexcept {
    if (!session_) {
        return;
    }
    session_->unmark_executed_modules(inserted_modules_);
    session_ = nullptr;
    inserted_modules_.clear();
}

CompilationSession::ActiveModuleExecutionGuard
CompilationSession::make_active_module_execution_guard() {
    ActiveModuleExecutionGuard guard(this);
    guard.inserted_modules_.reserve(
        guard.inserted_modules_.size() + active_module_init_stack_.size());
    for (const auto& active_module_name : active_module_init_stack_) {
        if (executed_modules_.insert(active_module_name).second) {
            guard.inserted_modules_.push_back(active_module_name);
        }
    }
    return guard;
}

void CompilationSession::unmark_executed_modules(
    const std::vector<std::string>& module_names) {
    for (const auto& module_name : module_names) {
        executed_modules_.erase(module_name);
    }
}

bool CompilationSession::clear_module_cache(const std::string& module_name) {
    bool changed = false;

    if (executed_modules_.erase(module_name) > 0) {
        changed = true;
    }

    if (etac_module_reservations_.erase(module_name) > 0) {
        changed = true;
    }
    for (auto it = etac_module_reservations_.begin();
         it != etac_module_reservations_.end();) {
        auto& reserve_modules = it->second;
        const auto before = reserve_modules.size();
        reserve_modules.erase(
            std::remove(
                reserve_modules.begin(),
                reserve_modules.end(),
                module_name),
            reserve_modules.end());
        if (reserve_modules.empty()) {
            it = etac_module_reservations_.erase(it);
            changed = true;
            continue;
        }
        if (reserve_modules.size() != before) {
            changed = true;
        }
        ++it;
    }

    const auto before_forms = accumulated_forms_.size();
    accumulated_forms_.erase(
        std::remove_if(
            accumulated_forms_.begin(),
            accumulated_forms_.end(),
            [&module_name](const reader::parser::SExprPtr& form) {
                auto* module = form->template as<reader::parser::ModuleForm>();
                if (module) return module->name == module_name;

                auto* lst = form->template as<reader::parser::List>();
                if (!lst || lst->elems.size() < 2) return false;
                if (!reader::utils::is_symbol_named(lst->elems[0], "module")) {
                    return false;
                }
                auto* name = lst->elems[1]->template as<reader::parser::Symbol>();
                return name && name->name == module_name;
            }),
        accumulated_forms_.end());
    if (accumulated_forms_.size() != before_forms) {
        changed = true;
    }

    const std::string prefix = module_name + ".";
    for (auto it = global_names_.begin(); it != global_names_.end();) {
        if (it->second == module_name || it->second.starts_with(prefix)) {
            it = global_names_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    if (runtime_module_info_.erase(module_name) > 0) {
        changed = true;
    }
    if (compiled_link_modules_.erase(module_name) > 0) {
        changed = true;
    }

    return changed;
}

std::vector<std::string> CompilationSession::collect_imported_modules(
    std::span<const reader::parser::SExprPtr> forms) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;

    auto extract_module_name = [](const reader::parser::SExprPtr& clause)
        -> std::string {
        namespace utils = reader::utils;
        if (auto s = utils::as_symbol(clause)) return s->name;
        if (auto l = utils::as_list(clause)) {
            if (l->elems.size() >= 2) {
                if (auto m = utils::as_symbol(l->elems[1])) return m->name;
            }
        }
        return {};
    };

    for (const auto& form : forms) {
        auto* lst = form ? form->template as<reader::parser::List>() : nullptr;
        if (!lst || lst->elems.size() < 2) continue;
        if (!reader::utils::is_symbol_named(lst->elems[0], "module")) continue;
        for (std::size_t i = 2; i < lst->elems.size(); ++i) {
            auto* inner =
                lst->elems[i] ? lst->elems[i]->template as<reader::parser::List>()
                              : nullptr;
            if (!inner || inner->elems.empty()) continue;
            if (!reader::utils::is_symbol_named(inner->elems[0], "import")) {
                continue;
            }
            for (std::size_t j = 1; j < inner->elems.size(); ++j) {
                auto name = extract_module_name(inner->elems[j]);
                if (!name.empty() && seen.insert(name).second) {
                    result.push_back(std::move(name));
                }
            }
        }
    }

    return result;
}

std::unordered_set<std::string> CompilationSession::collect_declared_module_names(
    std::span<const reader::parser::SExprPtr> forms) {
    std::unordered_set<std::string> names;
    for (const auto& form : forms) {
        auto* lst = form ? form->template as<reader::parser::List>() : nullptr;
        if (!lst || lst->elems.size() < 2) continue;
        if (!reader::utils::is_symbol_named(lst->elems[0], "module")) continue;
        auto* nsym =
            lst->elems[1] ? lst->elems[1]->template as<reader::parser::Symbol>()
                          : nullptr;
        if (!nsym || nsym->name.empty()) continue;
        names.insert(nsym->name);
    }
    return names;
}

bool CompilationSession::form_declares_module(
    const reader::parser::SExprPtr& form,
    const std::string& module_name) {
    auto* lst = form ? form->template as<reader::parser::List>() : nullptr;
    if (!lst || lst->elems.size() < 2) return false;
    if (!reader::utils::is_symbol_named(lst->elems[0], "module")) return false;
    auto* nsym = lst->elems[1]->template as<reader::parser::Symbol>();
    return nsym && nsym->name == module_name;
}

bool CompilationSession::module_declared(
    const std::string& module_name,
    std::span<const reader::parser::SExprPtr> new_forms) const {
    for (const auto& f : accumulated_forms_) {
        if (form_declares_module(f, module_name)) return true;
    }
    for (const auto& f : new_forms) {
        if (form_declares_module(f, module_name)) return true;
    }
    return false;
}

bool CompilationSession::auto_load_imports(
    Host& host,
    std::span<const reader::parser::SExprPtr> new_forms) {
    auto needed = collect_imported_modules(new_forms);
    for (const auto& mod_name : needed) {
        const bool already_accumulated = module_declared(mod_name, new_forms);
        if (already_accumulated) continue;

        if (executed_modules_.contains(mod_name)) {
            continue;
        }

        if (loading_modules_.contains(mod_name)) {
            std::string cycle;
            for (const auto& m : loading_modules_) {
                if (!cycle.empty()) cycle += " -> ";
                cycle += m;
            }
            cycle += " -> ";
            cycle += mod_name;
            host.diagnostics().emit_error(
                diagnostic::DiagnosticCode::ModuleNotFound,
                {},
                "circular module import detected: " + cycle);
            return false;
        }

        bool shadow_conflict = false;
        auto path = host.resolve_import_path(mod_name, &shadow_conflict);
        if (!path) {
            if (shadow_conflict) return false;
            continue;
        }

        auto canonical = path->string();
        if (loaded_files_.contains(canonical)) continue;

        loading_modules_.insert(mod_name);
        loaded_files_.insert(canonical);
        const bool ok = host.run_module_file(*path);
        loading_modules_.erase(mod_name);

        if (!ok) return false;
    }
    return true;
}

bool CompilationSession::run_source_impl(Host& host,
                                         const std::string& source,
                                         uint32_t file_id,
                                         runtime::nanbox::LispVal* result,
                                         const std::string& result_binding,
                                         bool execute,
                                         CompileResult* out_cr) {
    host.diagnostics().clear();

    reader::lexer::Lexer lex(file_id, source);
    reader::parser::Parser parser(lex);

    auto parsed_res = parser.parse_toplevel();
    if (!parsed_res) {
        auto& err = parsed_res.error();
        std::visit(
            [&host](auto&& e) {
                host.diagnostics().emit(diagnostic::to_diagnostic(e));
            },
            err);
        return false;
    }
    auto parsed = std::move(*parsed_res);
    if (parsed.empty()) {
        return true;
    }

    reader::expander::Expander expander;
    auto expanded_res = expander.expand_many(parsed);
    if (!expanded_res) {
        host.diagnostics().emit(diagnostic::to_diagnostic(expanded_res.error()));
        return false;
    }
    auto new_expanded = std::move(*expanded_res);

    std::vector<std::string> new_module_names;
    for (const auto& form : new_expanded) {
        if (auto* mf = form->template as<reader::parser::ModuleForm>()) {
            new_module_names.push_back(mf->name);
        } else if (auto* lst = form->template as<reader::parser::List>()) {
            if (!lst->elems.empty()) {
                if (auto* sym = lst->elems[0]->template as<reader::parser::Symbol>()) {
                    if (sym->name == "module" && lst->elems.size() >= 2) {
                        if (auto* nsym = lst->elems[1]->template as<reader::parser::Symbol>()) {
                            new_module_names.push_back(nsym->name);
                        }
                    }
                }
            }
        }
    }

    std::span<const reader::parser::SExprPtr> new_span(
        new_expanded.data(),
        new_expanded.size());
    if (!auto_load_imports(host, new_span)) {
        return false;
    }

    const std::size_t accumulated_forms_base = accumulated_forms_.size();
    const auto rollback_accumulated_forms = [&]() {
        accumulated_forms_.resize(accumulated_forms_base);
    };

    if (out_cr) {
        auto needed = collect_imported_modules(new_span);
        for (const auto& mod_name : needed) {
            bool defined_locally = false;
            for (const auto& nm : new_module_names) {
                if (nm == mod_name) {
                    defined_locally = true;
                    break;
                }
            }
            if (!defined_locally) {
                out_cr->imports.push_back(mod_name);
            }
        }
    }

    for (auto& f : new_expanded) {
        accumulated_forms_.push_back(reader::parser::deep_copy(f));
    }

    reader::ModuleLinker linker;
    auto idx_res = linker.index_modules(accumulated_forms_);
    if (!idx_res) {
        rollback_accumulated_forms();
        host.emit_link_error(idx_res.error());
        return false;
    }

    const auto source_declared_module_names =
        collect_declared_module_names(accumulated_forms_);
    std::vector<std::string> replay_compiled_modules;
    replay_compiled_modules.reserve(compiled_link_modules_.size());
    for (const auto& [module_name, _] : compiled_link_modules_) {
        if (source_declared_module_names.contains(module_name)) continue;
        replay_compiled_modules.push_back(module_name);
    }
    std::sort(replay_compiled_modules.begin(), replay_compiled_modules.end());
    for (const auto& module_name : replay_compiled_modules) {
        const auto it = compiled_link_modules_.find(module_name);
        if (it == compiled_link_modules_.end()) continue;
        auto replay_res = linker.index_compiled_module_exports(
            it->second.name,
            std::span<const std::string>(
                it->second.exports.data(),
                it->second.exports.size()));
        if (!replay_res) {
            rollback_accumulated_forms();
            host.emit_link_error(replay_res.error());
            return false;
        }
    }

    auto link_res = linker.link();
    if (!link_res) {
        rollback_accumulated_forms();
        host.emit_link_error(link_res.error());
        return false;
    }

    semantics::SemanticAnalyzer sa;
    auto sem_res = sa.analyze_all(
        accumulated_forms_,
        linker,
        host.builtins(),
        host.extensions(),
        [this](std::string_view module_name,
               std::string_view export_name) -> std::optional<uint32_t> {
            return runtime_export_slot(module_name, export_name);
        });
    if (!sem_res) {
        rollback_accumulated_forms();
        host.diagnostics().emit(diagnostic::to_diagnostic(sem_res.error()));
        return false;
    }
    auto sem_mods = std::move(*sem_res);
    if (sem_mods.empty()) return true;

    host.optimization_pipeline().run_all(sem_mods);

    auto& globals = host.vm().globals();
    const auto needed = sem_mods[0].total_globals;
    if (globals.size() < needed) {
        globals.resize(needed, runtime::nanbox::Nil);
    }

    if (execute) {
        auto install_res = host.primitive_installer().install_into(globals, needed);
        if (!install_res) {
            rollback_accumulated_forms();
            host.emit_runtime_error(install_res.error());
            return false;
        }
        host.primitive_installer().record_names(global_names_);
    }

    const uint32_t base_func_idx = static_cast<uint32_t>(host.registry().size());

    for (auto& mod : sem_mods) {
        if (executed_modules_.contains(mod.name)) {
            continue;
        }

        const uint32_t module_func_begin = static_cast<uint32_t>(host.registry().size());
        semantics::Emitter emitter(mod, host.heap(), host.intern_table(), host.registry());
        auto* init_func = emitter.emit();
        const uint32_t module_func_end = static_cast<uint32_t>(host.registry().size());
        const uint32_t primitive_slot_limit =
            static_cast<uint32_t>(host.total_primitive_count());

        for (const auto& bi : mod.bindings) {
            if (bi.kind == semantics::BindingInfo::Kind::Global && !bi.name.empty()) {
                if (bi.slot < primitive_slot_limit) continue;
                global_names_[bi.slot] = mod.name + "." + bi.name;
            }
        }

        if (out_cr) {
            CompileModuleEntry cme;
            cme.name = mod.name;
            cme.init_func_index =
                static_cast<uint32_t>(host.registry().size()) - 1 - base_func_idx;
            cme.total_globals = mod.total_globals;
            cme.main_func_slot = mod.main_func_slot;

            cme.first_func_index = module_func_begin - base_func_idx;
            cme.func_count = module_func_end - module_func_begin;

            for (const auto& bi : mod.bindings) {
                if (bi.kind == semantics::BindingInfo::Kind::Global
                    && bi.mutable_flag) {
                    cme.owned_global_slots.push_back(bi.slot);
                } else if (bi.kind == semantics::BindingInfo::Kind::Import
                           && bi.origin.has_value()) {
                    CompileModuleEntry::ImportBinding ib;
                    ib.local_slot = bi.slot;
                    ib.from_module = bi.origin->from_module;
                    ib.remote_name = bi.origin->remote_name;
                    cme.import_bindings.push_back(std::move(ib));
                }
            }

            std::sort(
                cme.owned_global_slots.begin(),
                cme.owned_global_slots.end());
            cme.owned_global_slots.erase(
                std::unique(
                    cme.owned_global_slots.begin(),
                    cme.owned_global_slots.end()),
                cme.owned_global_slots.end());

            cme.export_bindings.reserve(mod.exports.size());
            for (const auto& [export_name, binding_id] : mod.exports) {
                if (binding_id.id >= mod.bindings.size()) continue;
                CompileModuleEntry::ExportBinding eb;
                eb.name = export_name;
                eb.slot = mod.bindings[binding_id.id].slot;
                cme.export_bindings.push_back(std::move(eb));
            }
            std::sort(
                cme.export_bindings.begin(),
                cme.export_bindings.end(),
                [](const CompileModuleEntry::ExportBinding& lhs,
                   const CompileModuleEntry::ExportBinding& rhs) {
                    return lhs.name < rhs.name;
                });

            out_cr->modules.push_back(std::move(cme));
        }

        if (execute) {
            struct ActiveModuleInitGuard {
                std::vector<std::string>& active_module_init_stack;

                ActiveModuleInitGuard(
                    std::vector<std::string>& stack,
                    const std::string& module_name)
                    : active_module_init_stack(stack) {
                    active_module_init_stack.push_back(module_name);
                }

                ~ActiveModuleInitGuard() {
                    active_module_init_stack.pop_back();
                }
            };

            ActiveModuleInitGuard active_module_guard(active_module_init_stack_, mod.name);
            auto exec_res = host.vm().execute(*init_func);
            if (!exec_res) {
                rollback_accumulated_forms();
                host.emit_runtime_error(exec_res.error());
                return false;
            }

            executed_modules_.insert(mod.name);
            record_runtime_exports_from_source_module(mod);

            if (mod.main_func_slot) {
                auto main_val = globals[*mod.main_func_slot];
                if (main_val != runtime::nanbox::Nil) {
                    auto main_res = host.vm().call_value(main_val, {});
                    if (!main_res) {
                        rollback_accumulated_forms();
                        host.emit_runtime_error(main_res.error());
                        return false;
                    }
                }
            }

            if (result && !result_binding.empty()) {
                const bool is_last_new =
                    (!new_module_names.empty() && mod.name == new_module_names.back());
                if (is_last_new) {
                    for (const auto& bi : mod.bindings) {
                        if (bi.name == result_binding) {
                            *result = host.vm().globals()[bi.slot];
                            break;
                        }
                    }
                }
            }
        }
    }

    const uint32_t end_func_idx = static_cast<uint32_t>(host.registry().size());
    if (out_cr) {
        out_cr->base_func_idx = base_func_idx;
        out_cr->end_func_idx = end_func_idx;
    }

    return true;
}

bool CompilationSession::append_etac_global_reservation(
    Host& host,
    uint32_t slots_to_reserve,
    std::string* reserve_module_name) {
    if (slots_to_reserve == 0) return true;

    const std::string reserve_name =
        "__eta_etac_reserve_" + std::to_string(etac_reserve_counter_++);
    if (reserve_module_name != nullptr) {
        *reserve_module_name = reserve_name;
    }

    std::ostringstream source;
    source << "(module " << reserve_name << "\n";
    for (uint32_t i = 0; i < slots_to_reserve; ++i) {
        source << "  (define __eta_etac_slot_" << i << " '())\n";
    }
    source << ")";

    const std::string source_text = source.str();
    reader::lexer::Lexer lex(/*file_id=*/0, source_text);
    reader::parser::Parser parser(lex);
    auto parsed_res = parser.parse_toplevel();
    if (!parsed_res) {
        auto& err = parsed_res.error();
        std::visit(
            [&host](auto&& e) {
                host.diagnostics().emit(diagnostic::to_diagnostic(e));
            },
            err);
        return false;
    }

    reader::expander::Expander expander;
    auto expanded_res = expander.expand_many(*parsed_res);
    if (!expanded_res) {
        host.diagnostics().emit(diagnostic::to_diagnostic(expanded_res.error()));
        return false;
    }

    for (auto& f : *expanded_res) {
        accumulated_forms_.push_back(reader::parser::deep_copy(f));
    }
    executed_modules_.insert(reserve_name);
    return true;
}

void CompilationSession::drop_etac_reservation_modules(
    const std::vector<std::string>& reserve_modules) {
    for (const auto& reserve_module_name : reserve_modules) {
        (void)clear_module_cache(reserve_module_name);
    }
}

bool CompilationSession::release_etac_global_reservation(
    const std::string& module_name) {
    auto it = etac_module_reservations_.find(module_name);
    if (it == etac_module_reservations_.end()) return true;

    auto reserve_modules = std::move(it->second);
    etac_module_reservations_.erase(it);
    drop_etac_reservation_modules(reserve_modules);
    return true;
}

void CompilationSession::add_etac_module_reservation(
    const std::string& module_name,
    std::string reserve_module_name) {
    if (reserve_module_name.empty()) return;
    etac_module_reservations_[module_name].push_back(std::move(reserve_module_name));
}

bool CompilationSession::hydrate_executed_module_source(
    Host& host,
    const std::string& module_name) {
    bool shadow_conflict = false;
    auto resolved = host.resolve_import_path(module_name, &shadow_conflict);
    if (!resolved) return !shadow_conflict;

    std::filesystem::path source_path = *resolved;
    if (source_path.extension() == ".etac") {
        auto sibling_source = source_path;
        sibling_source.replace_extension(".eta");
        std::error_code ec;
        if (!std::filesystem::is_regular_file(sibling_source, ec) || ec) {
            return true;
        }
        source_path = sibling_source;
    } else if (source_path.extension() != ".eta") {
        return true;
    }

    const auto source_key = util::canonical_path_key(source_path);
    if (indexed_source_files_.contains(source_key)) return true;

    if (!release_etac_global_reservation(module_name)) return false;
    indexed_source_files_.insert(source_key);
    if (!host.run_module_file(source_path)) {
        indexed_source_files_.erase(source_key);
        return false;
    }
    return true;
}

void CompilationSession::record_runtime_exports_from_source_module(
    const semantics::ModuleSemantics& mod) {
    RuntimeModuleInfo info;
    for (const auto& [export_name, binding_id] : mod.exports) {
        if (binding_id.id >= mod.bindings.size()) continue;
        info.export_slots[export_name] = mod.bindings[binding_id.id].slot;
    }
    runtime_module_info_[mod.name] = std::move(info);
}

void CompilationSession::record_runtime_exports_from_compiled_module(
    const std::string& module_name,
    const std::unordered_map<std::string, uint32_t>& export_slots) {
    RuntimeModuleInfo info;
    info.export_slots = export_slots;
    runtime_module_info_[module_name] = std::move(info);
}

void CompilationSession::record_compiled_link_exports_from_compiled_module(
    const runtime::vm::ModuleEntry& module,
    const std::filesystem::path& artifact_path) {
    auto& info = compiled_link_modules_[module.name];
    info.name = module.name;
    info.artifact_path = artifact_path;
    info.exports.clear();
    info.exports.reserve(module.export_bindings.size());
    for (const auto& ex : module.export_bindings) {
        if (ex.name.empty()) continue;
        info.exports.push_back(ex.name);
    }
    std::sort(info.exports.begin(), info.exports.end());
    info.exports.erase(
        std::unique(info.exports.begin(), info.exports.end()),
        info.exports.end());
}

} // namespace eta::session
