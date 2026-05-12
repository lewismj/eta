/**
 * @file etac_loader.cpp
 * @brief `.etac` artifact loading and execution for eta sessions.
 */

#include "eta/session/etac_loader.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include "eta/diagnostic/diagnostic.h"
#include "eta/package/discovery.h"
#include "eta/runtime/embedded_prelude.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/vm/bytecode_serializer.h"
#include "eta/runtime/vm/vm.h"
#include "eta/session/runtime_primitives.h"

namespace eta::session {

namespace fs = std::filesystem;

fs::path EtacLoader::embedded_prelude_marker_path() {
    return fs::path("<embedded:prelude.etac>");
}

bool EtacLoader::try_load_embedded_prelude() {
    const auto blob = runtime::embedded_prelude_blob();
    if (blob.empty()) return false;

    std::string bytes;
    bytes.resize(blob.size());
    for (std::size_t i = 0; i < blob.size(); ++i) {
        bytes[i] = static_cast<char>(blob[i]);
    }

    std::istringstream in(bytes, std::ios::in | std::ios::binary);
    runtime::vm::BytecodeSerializer serializer(host_.heap(), host_.intern_table());
    auto etac_res = serializer.deserialize(
        in,
        static_cast<uint32_t>(host_.builtin_count()));
    if (!etac_res) {
        return false;
    }

    if (!execute_deserialized_etac(*etac_res, embedded_prelude_marker_path())) {
        host_.diagnostics().clear();
        return false;
    }
    return true;
}

bool EtacLoader::run_etac_file(const fs::path& path) {
    if (!host_.ensure_package_sidecars_loaded(path.parent_path())) {
        return false;
    }

    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) {
        host_.diagnostics().emit_error(
            diagnostic::DiagnosticCode::ModuleNotFound, {},
            "cannot open file: " + path.string());
        return false;
    }

    runtime::vm::BytecodeSerializer serializer(host_.heap(), host_.intern_table());
    auto etac_res = serializer.deserialize(in, /*expected_builtins=*/0);
    if (!etac_res) {
        host_.diagnostics().emit_error(
            diagnostic::DiagnosticCode::ModuleNotFound, {},
            "failed to load .etac: " + std::string(runtime::vm::to_string(etac_res.error())));
        return false;
    }
    auto& etac = *etac_res;

    runtime::vm::FreshnessContext freshness;
    freshness.expected_compiler_id = runtime::vm::BytecodeSerializer::default_compiler_id();
    freshness.expected_builtin_count = static_cast<uint32_t>(host_.builtin_count());
    freshness.expected_extension_env_hash = host_.extension_env_hash();

    auto sibling_source = path;
    sibling_source.replace_extension(".eta");
    if (std::error_code ec; fs::is_regular_file(sibling_source, ec) && !ec) {
        if (auto source_hash = hash_file_for_etac_freshness(sibling_source)) {
            freshness.expected_source_hash = *source_hash;
        }
    }

    if (auto manifest_path = package::find_nearest_manifest_path(path.parent_path())) {
        if (auto manifest_hash = hash_file_for_etac_freshness(*manifest_path)) {
            freshness.expected_manifest_hash = *manifest_hash;
        }
    }

    const auto freshness_result =
        runtime::vm::BytecodeSerializer::check_freshness(etac, freshness);
    if (!freshness_result.fresh()) {
        std::string message =
            "stale .etac detected: " + std::string(runtime::vm::to_string(freshness_result.status));
        if (!freshness_result.detail.empty()) {
            message += " (" + freshness_result.detail + ")";
        }

        std::error_code ec;
        if (fs::is_regular_file(sibling_source, ec) && !ec) {
            const bool fallback_ok = host_.run_source_file(sibling_source);
            host_.diagnostics().emit_warning(
                diagnostic::DiagnosticCode::ModuleNotFound, {},
                message + "; falling back to source: " + sibling_source.string());
            return fallback_ok;
        }

        host_.diagnostics().emit_error(
            diagnostic::DiagnosticCode::ModuleNotFound, {},
            message + "; no sibling source found for fallback");
        return false;
    }

    return execute_deserialized_etac(etac, path);
}

void EtacLoader::relocate_function_global_slots(
    runtime::vm::BytecodeFunction& func,
    const std::unordered_map<uint32_t, uint32_t>& slot_map) {
    if (slot_map.empty()) return;
    for (auto& instr : func.code) {
        if (instr.opcode != runtime::vm::OpCode::LoadGlobal
            && instr.opcode != runtime::vm::OpCode::StoreGlobal) {
            continue;
        }
        auto it = slot_map.find(instr.arg);
        if (it != slot_map.end()) {
            instr.arg = it->second;
        }
    }
}

bool EtacLoader::execute_deserialized_etac(runtime::vm::EtacFile& etac,
                                           const fs::path& artifact_path) {
    /// Auto-load non-prelude imports
    for (const auto& imp : etac.imports) {
        if (compilation_.has_module(imp)) continue;
        bool shadow_conflict = false;
        auto imp_path = host_.resolve_import_path(imp, &shadow_conflict);
        if (!imp_path) {
            if (!shadow_conflict) {
                host_.diagnostics().emit_error(
                    diagnostic::DiagnosticCode::ModuleNotFound, {},
                    "cannot resolve import '" + imp + "' required by .etac");
            }
            return false;
        }
        if (!host_.run_module_file(*imp_path)) return false;
    }

    if (etac.format_version < runtime::vm::BytecodeSerializer::FORMAT_VERSION_V5) {
        /**
         * Legacy path for v3/v4 artifacts that do not carry relocation
         * metadata. Keep existing behavior for backward compatibility.
         */
        uint32_t base_idx = static_cast<uint32_t>(host_.registry().size());
        for (const auto& func : etac.registry.all()) {
            runtime::vm::BytecodeFunction copy = func;
            copy.rebase_func_indices(static_cast<int32_t>(base_idx));
            host_.registry().add(std::move(copy));
        }

        uint32_t accounted_globals = static_cast<uint32_t>(host_.vm().globals().size());
        for (const auto& mod : etac.modules) {
            compilation_.record_compiled_link_exports_from_compiled_module(
                mod,
                artifact_path);

            if (compilation_.has_module(mod.name)) {
                if (mod.total_globals > accounted_globals) {
                    accounted_globals = mod.total_globals;
                }
                continue;
            }

            if (mod.total_globals > accounted_globals) {
                const auto reserve_slots = mod.total_globals - accounted_globals;
                std::string reserve_module_name;
                if (!compilation_.append_etac_global_reservation(
                        compilation_host_,
                        reserve_slots,
                        &reserve_module_name)) {
                    return false;
                }
                compilation_.add_etac_module_reservation(
                    mod.name,
                    std::move(reserve_module_name));
                accounted_globals = mod.total_globals;
            }

            auto& globals = host_.vm().globals();
            if (globals.size() < mod.total_globals) {
                globals.resize(mod.total_globals, runtime::nanbox::Nil);
            }

            if (!host_.primitive_installer().installed()) {
                auto install_res = host_.primitive_installer().install_into(
                    globals,
                    mod.total_globals);
                if (!install_res) {
                    host_.emit_runtime_error(install_res.error());
                    return false;
                }
                host_.primitive_installer().record_names(compilation_.mutable_global_names());
            }

            uint32_t func_idx = base_idx + mod.init_func_index;
            const auto* init_func = host_.registry().get(func_idx);
            if (!init_func) {
                host_.diagnostics().emit_error(
                    diagnostic::DiagnosticCode::ModuleNotFound, {},
                    "missing init function for module: " + mod.name);
                return false;
            }

            auto exec_res = host_.vm().execute(*init_func);
            if (!exec_res) {
                host_.emit_runtime_error(exec_res.error());
                return false;
            }

            compilation_.mark_module_executed(mod.name);
            std::unordered_map<std::string, uint32_t> legacy_export_slots;
            legacy_export_slots.reserve(mod.export_bindings.size());
            for (const auto& ex : mod.export_bindings) {
                legacy_export_slots[ex.name] = ex.slot;
            }
            compilation_.record_runtime_exports_from_compiled_module(
                mod.name,
                legacy_export_slots);

            if (mod.main_func_slot) {
                auto main_val = globals[*mod.main_func_slot];
                if (main_val != runtime::nanbox::Nil) {
                    auto main_res = host_.vm().call_value(main_val, {});
                    if (!main_res) {
                        host_.emit_runtime_error(main_res.error());
                        return false;
                    }
                }
            }
        }
        return true;
    }

    struct ModuleRelocationPlan {
        bool execute{true};
        std::unordered_map<uint32_t, uint32_t> slot_map;
        std::unordered_map<std::string, uint32_t> export_slots;
        std::optional<uint32_t> runtime_main_slot;
        uint32_t runtime_global_count{0};
    };

    std::unordered_map<std::string, ModuleRelocationPlan> plans;
    uint32_t next_runtime_slot = std::max<uint32_t>(
        static_cast<uint32_t>(host_.vm().globals().size()),
        static_cast<uint32_t>(host_.total_primitive_count()));

    auto resolve_export_runtime_slot =
        [&](const std::string& module_name,
            const std::string& export_name) -> std::optional<uint32_t> {
        if (auto pit = plans.find(module_name); pit != plans.end()) {
            if (auto it = pit->second.export_slots.find(export_name);
                it != pit->second.export_slots.end()) {
                return it->second;
            }
        }
        return compilation_.runtime_export_slot(module_name, export_name);
    };

    for (const auto& mod : etac.modules) {
        ModuleRelocationPlan plan;
        plan.execute = !compilation_.has_module(mod.name);

        if (!plan.execute) {
            if (const auto* existing = compilation_.runtime_module_info(mod.name)) {
                plan.export_slots = existing->export_slots;
            }
            plan.runtime_global_count = next_runtime_slot;
            plans.emplace(mod.name, std::move(plan));
            continue;
        }

        for (const auto& imp : mod.import_bindings) {
            auto runtime_slot = resolve_export_runtime_slot(
                imp.from_module, imp.remote_name);
            if (!runtime_slot.has_value()) {
                host_.diagnostics().emit_error(
                    diagnostic::DiagnosticCode::ModuleNotFound, {},
                    "cannot relocate import '" + imp.remote_name
                        + "' from module '" + imp.from_module
                        + "' while loading '" + mod.name + "'");
                return false;
            }

            auto [it, inserted] = plan.slot_map.emplace(imp.local_slot, *runtime_slot);
            if (!inserted && it->second != *runtime_slot) {
                host_.diagnostics().emit_error(
                    diagnostic::DiagnosticCode::ModuleNotFound, {},
                    "inconsistent relocation for slot " + std::to_string(imp.local_slot)
                        + " while loading '" + mod.name + "'");
                return false;
            }
        }

        auto owned_slots = mod.owned_global_slots;
        std::sort(owned_slots.begin(), owned_slots.end());
        owned_slots.erase(
            std::unique(owned_slots.begin(), owned_slots.end()),
            owned_slots.end());
        for (const auto slot : owned_slots) {
            auto [_, inserted] = plan.slot_map.emplace(slot, next_runtime_slot);
            if (inserted) {
                ++next_runtime_slot;
            }
        }

        for (const auto& ex : mod.export_bindings) {
            auto it = plan.slot_map.find(ex.slot);
            const uint32_t runtime_slot =
                (it != plan.slot_map.end()) ? it->second : ex.slot;
            plan.export_slots[ex.name] = runtime_slot;
        }

        if (mod.main_func_slot.has_value()) {
            auto it = plan.slot_map.find(*mod.main_func_slot);
            plan.runtime_main_slot =
                (it != plan.slot_map.end()) ? it->second : *mod.main_func_slot;
        }

        plan.runtime_global_count = next_runtime_slot;
        plans.emplace(mod.name, std::move(plan));
    }

    const auto& file_funcs = etac.registry.all();
    std::vector<const runtime::vm::ModuleEntry*> owner_by_func(file_funcs.size(), nullptr);
    for (const auto& mod : etac.modules) {
        const uint64_t begin = mod.first_func_index;
        const uint64_t end = begin + mod.func_count;
        if (end > file_funcs.size()) {
            host_.diagnostics().emit_error(
                diagnostic::DiagnosticCode::ModuleNotFound, {},
                "invalid function range in .etac module metadata for '" + mod.name + "'");
            return false;
        }
        for (uint64_t i = begin; i < end; ++i) {
            if (owner_by_func[static_cast<std::size_t>(i)] != nullptr) {
                host_.diagnostics().emit_error(
                    diagnostic::DiagnosticCode::ModuleNotFound, {},
                    "overlapping function ranges in .etac module metadata");
                return false;
            }
            owner_by_func[static_cast<std::size_t>(i)] = &mod;
        }
    }

    /**
     * Move functions from the deserialized registry into ours,
     * recording the base index so module init_func_index values can be offset.
     * The .etac stores 0-based (file-relative) function indices; relocate
     * them to the runner's absolute indices.
     */
    uint32_t base_idx = static_cast<uint32_t>(host_.registry().size());
    for (std::size_t i = 0; i < file_funcs.size(); ++i) {
        runtime::vm::BytecodeFunction copy = file_funcs[i];
        copy.rebase_func_indices(static_cast<int32_t>(base_idx));

        if (owner_by_func[i] != nullptr) {
            const auto plan_it = plans.find(owner_by_func[i]->name);
            if (plan_it != plans.end()) {
                relocate_function_global_slots(copy, plan_it->second.slot_map);
            }
        }

        host_.registry().add(std::move(copy));
    }

    uint32_t accounted_globals = static_cast<uint32_t>(host_.vm().globals().size());

    /// Execute each module's _init function
    for (const auto& mod : etac.modules) {
        const auto plan_it = plans.find(mod.name);
        if (plan_it == plans.end()) {
            host_.diagnostics().emit_error(
                diagnostic::DiagnosticCode::ModuleNotFound, {},
                "missing relocation plan for module: " + mod.name);
            return false;
        }
        const auto& plan = plan_it->second;

        if (!plan.execute) {
            if (plan.runtime_global_count > accounted_globals) {
                accounted_globals = plan.runtime_global_count;
            }
            continue;
        }

        if (plan.runtime_global_count > accounted_globals) {
            const auto reserve_slots = plan.runtime_global_count - accounted_globals;
            std::string reserve_module_name;
            if (!compilation_.append_etac_global_reservation(
                    compilation_host_,
                    reserve_slots,
                    &reserve_module_name)) {
                return false;
            }
            compilation_.add_etac_module_reservation(
                mod.name,
                std::move(reserve_module_name));
            accounted_globals = plan.runtime_global_count;
        }

        auto& globals = host_.vm().globals();
        if (globals.size() < plan.runtime_global_count) {
            globals.resize(plan.runtime_global_count, runtime::nanbox::Nil);
        }

        /// Re-install core and extension primitives
        if (!host_.primitive_installer().installed()) {
            auto install_res = host_.primitive_installer().install_into(
                globals,
                plan.runtime_global_count);
            if (!install_res) {
                host_.emit_runtime_error(install_res.error());
                return false;
            }
            host_.primitive_installer().record_names(compilation_.mutable_global_names());
        }

        uint32_t func_idx = base_idx + mod.init_func_index;
        const auto* init_func = host_.registry().get(func_idx);
        if (!init_func) {
            host_.diagnostics().emit_error(
                diagnostic::DiagnosticCode::ModuleNotFound, {},
                "missing init function for module: " + mod.name);
            return false;
        }

        auto exec_res = host_.vm().execute(*init_func);
        if (!exec_res) {
            host_.emit_runtime_error(exec_res.error());
            return false;
        }

        compilation_.mark_module_executed(mod.name);
        compilation_.record_runtime_exports_from_compiled_module(mod.name, plan.export_slots);
        const uint32_t primitive_slot_limit =
            static_cast<uint32_t>(host_.total_primitive_count());
        for (const auto& [export_name, slot] : plan.export_slots) {
            if (slot < primitive_slot_limit) continue;
            compilation_.mutable_global_names()[slot] = mod.name + "." + export_name;
        }

        /// Invoke optional main
        if (plan.runtime_main_slot) {
            auto main_val = globals[*plan.runtime_main_slot];
            if (main_val != runtime::nanbox::Nil) {
                auto main_res = host_.vm().call_value(main_val, {});
                if (!main_res) {
                    host_.emit_runtime_error(main_res.error());
                    return false;
                }
            }
        }
    }

    for (const auto& mod : etac.modules) {
        compilation_.record_compiled_link_exports_from_compiled_module(mod, artifact_path);
    }

    return true;
}

std::optional<uint64_t> EtacLoader::hash_file_for_etac_freshness(
    const fs::path& file_path) {
    std::ifstream in(file_path, std::ios::in | std::ios::binary);
    if (!in) return std::nullopt;

    std::ostringstream buf;
    buf << in.rdbuf();
    return runtime::vm::BytecodeSerializer::hash_source(buf.str());
}

} // namespace eta::session

