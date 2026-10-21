/// DAP custom inspectors: environment, local memory, heap, objects, disassembly.
#include "dap_server.h"
#include "dap_io.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>

/// Eta interpreter
#include "eta/session/driver.h"
#include "eta/session/bootstrap_module.h"
#include "eta/interpreter/module_path.h"
#include "eta/docs/markdown.h"
#include "eta/docs/stdlib_docs.h"
#include "eta/reader/special_form_docs.h"
#include "eta/runtime/builtin_metadata.h"
#include "eta/runtime/port.h"
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/vm/disassembler.h"
#include "eta/runtime/vm/sandbox.h"
#include "eta/runtime/value_formatter.h"
#include "eta/runtime/actor/actor_system.h"
#include "eta/runtime/memory/mark_sweep_gc.h"
#include "eta/runtime/memory/cons_pool.h"
#include "eta/runtime/memory/native_object_inspection.h"
#include "eta/runtime/types/types.h"
#include "eta/diagnostic/diagnostic.h"

#ifdef ETA_HAS_NNG
#include "eta/nng/nng_socket_ptr.h"
#include "eta/nng/nng_primitives.h"
#include "dap_server_internal.h"
#endif

namespace eta::dap {

namespace fs = std::filesystem;
using namespace eta::json;

/**
 */
void DapServer::handle_environment(const Value& id, const Value& args) {
    std::lock_guard<std::mutex> lk(vm_mutex_);

    const auto parse_limit = [](const Value& obj, const char* key, int64_t fallback) -> int64_t {
        if (obj.is_object()) {
            if (auto v = obj.get_int(key)) {
                return std::clamp<int64_t>(*v, 0, 5000);
            }
        }
        return fallback;
    };
    const auto parse_bool = [](const Value& obj, const char* key, bool fallback) -> bool {
        if (obj.is_object() && obj.has(key) && obj[key].is_bool()) {
            return obj[key].as_bool();
        }
        return fallback;
    };

    int frame_idx = 0;
    if (auto v = args.get_int("frameIndex")) {
        frame_idx = std::max<int>(0, static_cast<int>(*v));
    }

    Value include_obj;
    if (args.is_object() && args.has("include") && args["include"].is_object()) {
        include_obj = args["include"];
    }
    const bool include_locals = parse_bool(include_obj, "locals", true);
    const bool include_closures = parse_bool(include_obj, "closures", true);
    const bool include_globals = parse_bool(include_obj, "globals", false);
    const bool include_builtins = parse_bool(include_obj, "builtins", false);
    const bool include_internal = parse_bool(include_obj, "internal", false);
    const bool include_nil = parse_bool(include_obj, "nil", false);

    Value limits_obj;
    if (args.is_object() && args.has("limits") && args["limits"].is_object()) {
        limits_obj = args["limits"];
    }
    const int64_t max_locals = parse_limit(limits_obj, "maxLocals", 200);
    const int64_t max_closures = parse_limit(limits_obj, "maxClosures", 200);
    const int64_t max_globals = parse_limit(limits_obj, "maxGlobals", 200);
    const int64_t max_builtins = parse_limit(limits_obj, "maxBuiltins", 200);

    DapThread* th = resolve_thread_arg_locked(args);
    const int resolved_thread_id = th ? th->dap_thread_id : MAIN_THREAD_ID;

    auto make_empty_environments = [&](const std::string& module_name) -> Array {
        Array envs;
        int64_t depth = 0;
        auto append = [&](const char* kind, const std::string& label) {
            envs.push_back(json::object({
                {"kind", Value(kind)},
                {"label", Value(label)},
                {"depth", Value(depth++)},
                {"total", 0},
                {"truncated", false},
                {"bindings", json::array({})},
            }));
        };
        if (include_locals) {
            append("locals", "Frame locals");
        }
        if (include_closures) {
            append("closure", "Closure parent #1");
        }
        if (include_globals) {
            append("module", module_name.empty() ? "Module" : ("Module (" + module_name + ")"));
        }
        if (include_builtins) {
            append("builtins", "Builtins");
        }
        return envs;
    };
    auto send_empty = [&](const std::string& frame_name = std::string{},
                          const std::string& module_name = std::string{}) {
        send_response(id, json::object({
            {"threadId", Value(static_cast<int64_t>(resolved_thread_id))},
            {"frameIndex", Value(static_cast<int64_t>(frame_idx))},
            {"frameName", Value(frame_name)},
            {"moduleName", Value(module_name)},
            {"environments", Value(make_empty_environments(module_name))},
        }));
    };

    if (!th || !th->vm || !th->driver || !th->vm->is_paused()) {
        send_empty();
        return;
    }

    session::Driver& drv = *th->driver;
    runtime::vm::VM& vm = *th->vm;
    const auto frames = vm.get_frames();
    if (frame_idx < 0 || static_cast<std::size_t>(frame_idx) >= frames.size()) {
        send_empty();
        return;
    }

    const auto& frame = frames[static_cast<std::size_t>(frame_idx)];
    const std::string frame_name = frame.func_name.empty() ? "<anonymous>" : frame.func_name;
    const std::string module_name = current_module_from_frame(drv, static_cast<std::size_t>(frame_idx));

    auto collect_bindings = [&](const auto& entries,
                                int64_t limit,
                                int64_t& out_total,
                                bool& out_truncated) -> Array {
        Array vars;
        out_total = 0;
        out_truncated = false;
        for (const auto& e : entries) {
            const bool is_internal = !e.name.empty() && e.name[0] == '%';
            if (!include_internal && is_internal) continue;
            const bool is_nil = (e.value == runtime::nanbox::Nil);
            if (!include_nil && is_nil) continue;
            out_total++;
            if (limit > 0 && static_cast<int64_t>(vars.size()) >= limit) {
                out_truncated = true;
                continue;
            }
            vars.push_back(make_variable_json(drv, th->dap_thread_id, e.name, e.value));
        }
        return vars;
    };

    auto collect_slots = [&](const std::vector<std::pair<std::string, uint32_t>>& slots,
                             int64_t limit,
                             int64_t& out_total,
                             bool& out_truncated) -> Array {
        Array vars;
        out_total = 0;
        out_truncated = false;
        const auto& globals = vm.globals();
        for (const auto& [name, slot] : slots) {
            if (slot >= globals.size()) continue;
            const auto value = globals[slot];
            out_total++;
            if (limit > 0 && static_cast<int64_t>(vars.size()) >= limit) {
                out_truncated = true;
                continue;
            }
            vars.push_back(make_variable_json(drv, th->dap_thread_id, name, value));
        }
        return vars;
    };

    Array environments;
    int64_t lexical_depth = 0;
    auto append_environment = [&](const char* kind,
                                  const std::string& label,
                                  int64_t total,
                                  bool truncated,
                                  Array bindings) {
        environments.push_back(json::object({
            {"kind", Value(kind)},
            {"label", Value(label)},
            {"depth", Value(lexical_depth++)},
            {"total", Value(total)},
            {"truncated", Value(truncated)},
            {"bindings", Value(std::move(bindings))},
        }));
    };

    if (include_locals) {
        const auto locals = vm.get_locals(static_cast<std::size_t>(frame_idx));
        int64_t total = 0;
        bool truncated = false;
        auto vars = collect_bindings(locals, max_locals, total, truncated);
        append_environment("locals", "Frame locals", total, truncated, std::move(vars));
    }

    if (include_closures) {
        const auto closure_opt = vm.get_frame_closure(static_cast<std::size_t>(frame_idx));
        auto& heap = drv.heap();
        std::unordered_set<runtime::memory::heap::ObjectId> seen_closures;
        runtime::nanbox::LispVal closure_val = closure_opt.value_or(runtime::nanbox::Nil);
        int closure_level = 0;
        while (runtime::nanbox::ops::is_boxed(closure_val)
               && runtime::nanbox::ops::tag(closure_val) == runtime::nanbox::Tag::HeapObject) {
            const auto oid = static_cast<runtime::memory::heap::ObjectId>(
                runtime::nanbox::ops::payload(closure_val));
            if (!seen_closures.insert(oid).second) {
                break;
            }
            auto* cl = heap.try_get_as<
                runtime::memory::heap::ObjectKind::Closure,
                runtime::types::Closure>(oid);
            if (!cl) {
                break;
            }

            std::vector<runtime::vm::VarEntry> upvalue_entries;
            upvalue_entries.reserve(cl->upvals.size());
            for (std::size_t i = 0; i < cl->upvals.size(); ++i) {
                runtime::vm::VarEntry e;
                if (cl->func
                    && i < cl->func->upval_names.size()
                    && !cl->func->upval_names[i].empty()) {
                    e.name = cl->func->upval_names[i];
                } else {
                    e.name = "&" + std::to_string(i);
                }
                e.value = cl->upvals[i];
                upvalue_entries.push_back(std::move(e));
            }

            int64_t total = 0;
            bool truncated = false;
            auto vars = collect_bindings(upvalue_entries, max_closures, total, truncated);
            append_environment(
                "closure",
                "Closure parent #" + std::to_string(++closure_level),
                total,
                truncated,
                std::move(vars));

            if (cl->parent == runtime::nanbox::Nil) {
                break;
            }
            closure_val = cl->parent;
        }
        if (closure_level == 0) {
            append_environment("closure", "Closure parent #1", 0, false, Array{});
        }
    }

    if (include_globals || include_builtins) {
        const auto& globals = vm.globals();
        const auto& names = drv.global_names();
        std::vector<std::pair<std::string, uint32_t>> module_slots;
        std::vector<std::pair<std::string, uint32_t>> builtin_slots;
        module_slots.reserve(names.size());
        builtin_slots.reserve(names.size());

        for (const auto& [slot, full_name] : names) {
            if (slot >= globals.size()) continue;
            const auto value = globals[slot];
            const bool is_nil = (value == runtime::nanbox::Nil);
            if (!include_nil && is_nil) continue;

            const auto dot = full_name.rfind('.');
            if (dot == std::string::npos) {
                const bool is_internal = !full_name.empty() && full_name[0] == '%';
                if (!include_internal && is_internal) continue;
                if (include_builtins) {
                    builtin_slots.push_back({full_name, slot});
                }
                continue;
            }

            std::string short_name = full_name.substr(dot + 1);
            const bool is_internal = !short_name.empty() && short_name[0] == '%';
            if (!include_internal && is_internal) continue;

            const bool is_current_module =
                !module_name.empty()
                && dot == module_name.size()
                && full_name.compare(0, dot, module_name) == 0;
            if (is_current_module) {
                if (include_globals) {
                    module_slots.push_back({std::move(short_name), slot});
                }
                continue;
            }

            if (include_builtins) {
                /// Keep module prefix so similarly named imports remain distinct.
                builtin_slots.push_back({full_name, slot});
            }
        }

        std::sort(
            module_slots.begin(),
            module_slots.end(),
            [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return a.second < b.second;
            }
        );
        std::sort(
            builtin_slots.begin(),
            builtin_slots.end(),
            [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return a.second < b.second;
            }
        );

        if (include_globals) {
            int64_t total = 0;
            bool truncated = false;
            auto vars = collect_slots(module_slots, max_globals, total, truncated);
            append_environment(
                "module",
                module_name.empty() ? "Module" : ("Module (" + module_name + ")"),
                total,
                truncated,
                std::move(vars));
        }
        if (include_builtins) {
            int64_t total = 0;
            bool truncated = false;
            auto vars = collect_slots(builtin_slots, max_builtins, total, truncated);
            append_environment("builtins", "Builtins", total, truncated, std::move(vars));
        }
    }

    send_response(id, json::object({
        {"threadId", Value(static_cast<int64_t>(th->dap_thread_id))},
        {"frameIndex", Value(static_cast<int64_t>(frame_idx))},
        {"frameName", Value(frame_name)},
        {"moduleName", Value(module_name)},
        {"environments", Value(std::move(environments))},
    }));
}

/**
 */
void DapServer::handle_local_memory(const Value& id, const Value& args) {
    std::lock_guard<std::mutex> lk(vm_mutex_);

    int frame_idx = 0;
    if (auto v = args.get_int("frameIndex")) {
        frame_idx = std::max<int>(0, static_cast<int>(*v));
    }

    auto parse_limit = [&](const char* key, int64_t fallback) -> int64_t {
        if (auto v = args.get_int(key)) {
            return std::clamp<int64_t>(*v, 0, 5000);
        }
        return fallback;
    };
    const int64_t max_locals = parse_limit("maxLocals", 200);
    const int64_t max_upvalues = parse_limit("maxUpvalues", 200);
    const int64_t max_module_globals = parse_limit("maxModuleGlobals", 200);
    bool include_module_globals = true;
    if (args.has("includeModuleGlobals") && args["includeModuleGlobals"].is_bool()) {
        include_module_globals = args["includeModuleGlobals"].as_bool();
    }

    DapThread* th = resolve_thread_arg_locked(args);
    const int resolved_thread_id = th ? th->dap_thread_id : MAIN_THREAD_ID;
    auto send_empty = [&](const std::string& frame_name = std::string{},
                          const std::string& module_name = std::string{}) {
        send_response(id, json::object({
            {"threadId", Value(static_cast<int64_t>(resolved_thread_id))},
            {"frameIndex", Value(static_cast<int64_t>(frame_idx))},
            {"frameName", Value(frame_name)},
            {"moduleName", Value(module_name)},
            {"locals", json::array({})},
            {"upvalues", json::array({})},
            {"moduleGlobals", json::array({})},
            {"localsTotal", 0},
            {"upvaluesTotal", 0},
            {"moduleGlobalsTotal", 0},
            {"localsTruncated", false},
            {"upvaluesTruncated", false},
            {"moduleGlobalsTruncated", false},
        }));
    };

    if (!th || !th->vm || !th->driver || !th->vm->is_paused()) {
        send_empty();
        return;
    }

    session::Driver& drv = *th->driver;
    runtime::vm::VM& vm = *th->vm;
    const auto frames = vm.get_frames();
    if (frame_idx < 0 || static_cast<std::size_t>(frame_idx) >= frames.size()) {
        send_empty();
        return;
    }

    const auto& frame = frames[static_cast<std::size_t>(frame_idx)];
    const std::string frame_name = frame.func_name.empty() ? "<anonymous>" : frame.func_name;
    const std::string module_name = current_module_from_frame(drv, static_cast<std::size_t>(frame_idx));

    auto collect_bindings = [&](const auto& entries,
                                int64_t limit,
                                bool hide_internal_locals,
                                int64_t& out_total,
                                bool& out_truncated) -> Array {
        Array vars;
        out_total = 0;
        out_truncated = false;
        for (const auto& e : entries) {
            if (hide_internal_locals
                && !e.name.empty()
                && e.name[0] == '%'
                && e.value == runtime::nanbox::Nil) {
                continue;
            }
            out_total++;
            if (limit > 0 && static_cast<int64_t>(vars.size()) >= limit) {
                out_truncated = true;
                continue;
            }
            vars.push_back(make_variable_json(drv, th->dap_thread_id, e.name, e.value));
        }
        return vars;
    };

    int64_t locals_total = 0;
    int64_t upvalues_total = 0;
    int64_t module_globals_total = 0;
    bool locals_truncated = false;
    bool upvalues_truncated = false;
    bool module_globals_truncated = false;

    const auto locals = vm.get_locals(static_cast<std::size_t>(frame_idx));
    const auto upvalues = vm.get_upvalues(static_cast<std::size_t>(frame_idx));
    Array locals_vars = collect_bindings(
        locals, max_locals, true, locals_total, locals_truncated
    );
    Array upvalues_vars = collect_bindings(
        upvalues, max_upvalues, false, upvalues_total, upvalues_truncated
    );

    Array module_globals_vars;
    if (include_module_globals && !module_name.empty()) {
        const auto& globals = vm.globals();
        const auto& names = drv.global_names();
        std::vector<std::pair<std::string, uint32_t>> module_slots;
        module_slots.reserve(names.size());
        for (const auto& [slot, full_name] : names) {
            const auto dot = full_name.rfind('.');
            if (dot == std::string::npos) continue;
            if (full_name.substr(0, dot) != module_name) continue;
            module_slots.push_back({full_name.substr(dot + 1), slot});
        }
        std::sort(
            module_slots.begin(),
            module_slots.end(),
            [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return a.second < b.second;
            }
        );
        for (const auto& [name, slot] : module_slots) {
            if (slot >= globals.size()) continue;
            const auto v = globals[slot];
            if (v == runtime::nanbox::Nil) continue;
            module_globals_total++;
            if (max_module_globals > 0
                && static_cast<int64_t>(module_globals_vars.size()) >= max_module_globals) {
                module_globals_truncated = true;
                continue;
            }
            module_globals_vars.push_back(make_variable_json(drv, th->dap_thread_id, name, v));
        }
    }

    send_response(id, json::object({
        {"threadId", Value(static_cast<int64_t>(th->dap_thread_id))},
        {"frameIndex", Value(static_cast<int64_t>(frame_idx))},
        {"frameName", Value(frame_name)},
        {"moduleName", Value(module_name)},
        {"locals", Value(std::move(locals_vars))},
        {"upvalues", Value(std::move(upvalues_vars))},
        {"moduleGlobals", Value(std::move(module_globals_vars))},
        {"localsTotal", Value(locals_total)},
        {"upvaluesTotal", Value(upvalues_total)},
        {"moduleGlobalsTotal", Value(module_globals_total)},
        {"localsTruncated", Value(locals_truncated)},
        {"upvaluesTruncated", Value(upvalues_truncated)},
        {"moduleGlobalsTruncated", Value(module_globals_truncated)},
    }));
}

/**
 */

void DapServer::handle_heap_inspector(const Value& id, const Value& args) {
    if (id.is_int()) {
        const int64_t request_id = id.as_int();
        bool pre_cancelled = false;
        {
            std::lock_guard<std::mutex> lk(cancel_mutex_);
            auto it = cancelled_request_ids_.find(request_id);
            if (it != cancelled_request_ids_.end()) {
                pre_cancelled = true;
                cancelled_request_ids_.erase(it);
            }
        }
        if (pre_cancelled) {
            active_heap_snapshot_request_.store(-1, std::memory_order_relaxed);
            cancel_active_heap_snapshot_.store(false, std::memory_order_relaxed);
            send_error_response(id, 2020, "Request cancelled");
            return;
        }
        active_heap_snapshot_request_.store(request_id, std::memory_order_relaxed);
    } else {
        active_heap_snapshot_request_.store(-1, std::memory_order_relaxed);
    }
    cancel_active_heap_snapshot_.store(false, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (!driver_) {
        active_heap_snapshot_request_.store(-1, std::memory_order_relaxed);
        send_error_response(id, 2001, "VM not running");
        return;
    }
    if (!driver_->vm().is_paused()) {
        active_heap_snapshot_request_.store(-1, std::memory_order_relaxed);
        send_error_response(id, 2002, "VM must be paused to inspect the heap");
        return;
    }
    HeapSnapshotOptions opts;
    if (args.is_object()) {
        if (args.has("includeKinds") && args["includeKinds"].is_bool()) {
            opts.include_kinds = args["includeKinds"].as_bool();
        }
        if (args.has("includeRoots") && args["includeRoots"].is_bool()) {
            opts.include_roots = args["includeRoots"].as_bool();
        }
        if (auto v = args.get_int("maxObjectsScanned")) {
            opts.max_objects_scanned = std::max<int64_t>(0, *v);
        }
        if (auto v = args.get_int("maxKindRows")) {
            opts.max_kind_rows = std::max<int64_t>(0, *v);
        }
        if (auto v = args.get_int("maxRootsPerCategory")) {
            opts.max_roots_per_category = std::max<int64_t>(0, *v);
        }
    }

    bool cancelled = false;
    auto body = build_heap_snapshot(opts, &cancelled);
    active_heap_snapshot_request_.store(-1, std::memory_order_relaxed);
    if (cancelled) {
        send_error_response(id, 2020, "Request cancelled");
        return;
    }
    send_response(id, body);
}

Value DapServer::build_heap_snapshot(const HeapSnapshotOptions& opts, bool* out_cancelled) {
    using namespace runtime::memory::heap;
    if (out_cancelled) *out_cancelled = false;

    auto& heap = driver_->heap();
    bool truncated = false;
    bool kinds_truncated = false;
    int64_t kinds_total_rows = 0;

    /// Per-kind statistics
    struct KindStat { int64_t count{0}; int64_t bytes{0}; };
    std::unordered_map<std::string, KindStat> kind_stats;
    std::unordered_map<std::string, KindStat> native_type_stats;
    int64_t scanned_objects = 0;

    struct HeapSnapshotCancelled {};
    if (opts.include_kinds) {
        try {
            heap.for_each_entry([&](ObjectId /*id*/, HeapEntry& entry) {
                if (cancel_active_heap_snapshot_.load(std::memory_order_relaxed)) {
                    throw HeapSnapshotCancelled{};
                }
                if (opts.max_objects_scanned > 0 && scanned_objects >= opts.max_objects_scanned) {
                    truncated = true;
                    return;
                }
                scanned_objects++;
                const auto kind_label = heap_entry_kind_label(entry);
                kind_stats[kind_label].count++;
                kind_stats[kind_label].bytes += static_cast<int64_t>(entry.size);

                if (entry.header.kind != ObjectKind::NativeObject) return;

                std::string native_type_name = "<unnamed>";
                if (auto type_name = native_object_type_name(entry); !type_name.empty()) {
                    native_type_name = type_name;
                }
                native_type_stats[native_type_name].count++;
                native_type_stats[native_type_name].bytes += static_cast<int64_t>(entry.size);
            });
        } catch (const HeapSnapshotCancelled&) {
            if (out_cancelled) *out_cancelled = true;
            return json::object({});
        }
    }

    Array kinds_arr;
    Array native_types_arr;
    if (opts.include_kinds) {
        std::vector<std::pair<std::string, KindStat>> rows;
        rows.reserve(kind_stats.size());
        for (const auto& kv : kind_stats) rows.push_back(kv);
        std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) {
                if (a.second.bytes != b.second.bytes) return a.second.bytes > b.second.bytes;
                if (a.second.count != b.second.count) return a.second.count > b.second.count;
                return a.first < b.first;
            });
        kinds_total_rows = static_cast<int64_t>(rows.size());
        if (opts.max_kind_rows > 0
            && static_cast<int64_t>(rows.size()) > opts.max_kind_rows) {
            rows.resize(static_cast<std::size_t>(opts.max_kind_rows));
            truncated = true;
            kinds_truncated = true;
        }
        for (const auto& [kind_label, stat] : rows) {
            kinds_arr.push_back(json::object({
                {"kind",  Value(kind_label)},
                {"count", Value(stat.count)},
                {"bytes", Value(stat.bytes)},
            }));
        }

        std::vector<std::pair<std::string, KindStat>> native_rows;
        native_rows.reserve(native_type_stats.size());
        for (const auto& kv : native_type_stats) native_rows.push_back(kv);
        std::sort(native_rows.begin(), native_rows.end(),
            [](const auto& a, const auto& b) {
                if (a.second.bytes != b.second.bytes) return a.second.bytes > b.second.bytes;
                if (a.second.count != b.second.count) return a.second.count > b.second.count;
                return a.first < b.first;
            });
        for (const auto& [type_name, stat] : native_rows) {
            native_types_arr.push_back(json::object({
                {"typeName", Value(type_name)},
                {"count", Value(stat.count)},
                {"bytes", Value(stat.bytes)},
            }));
        }
    }

    /// GC roots
    Array roots_arr;
    if (opts.include_roots) {
        const auto gc_roots = driver_->vm().enumerate_gc_roots();
        const auto& names = driver_->global_names();

        std::unordered_map<runtime::memory::heap::ObjectId, uint32_t> slot_by_oid;
        bool need_global_labels = false;
        for (const auto& root : gc_roots) {
            if (root.name == "Globals") {
                need_global_labels = true;
                break;
            }
        }
        if (need_global_labels) {
            auto& globals = driver_->vm().globals();
            slot_by_oid.reserve(globals.size());
            for (std::size_t slot = 0; slot < globals.size(); ++slot) {
                auto v = globals[slot];
                if (!runtime::nanbox::ops::is_boxed(v)
                    || runtime::nanbox::ops::tag(v) != runtime::nanbox::Tag::HeapObject) {
                    continue;
                }
                auto oid = static_cast<runtime::memory::heap::ObjectId>(
                    runtime::nanbox::ops::payload(v)
                );
                slot_by_oid.emplace(oid, static_cast<uint32_t>(slot));
            }
        }

        for (const auto& root : gc_roots) {
            if (cancel_active_heap_snapshot_.load(std::memory_order_relaxed)) {
                if (out_cancelled) *out_cancelled = true;
                return json::object({});
            }

            Array ids;
            Array labels;
            const std::size_t total = root.object_ids.size();
            std::size_t limit = total;
            bool root_truncated = false;
            if (opts.max_roots_per_category > 0
                && static_cast<int64_t>(total) > opts.max_roots_per_category) {
                limit = static_cast<std::size_t>(opts.max_roots_per_category);
                root_truncated = true;
                truncated = true;
            }

            ids.reserve(limit);
            for (std::size_t i = 0; i < limit; ++i) {
                if (cancel_active_heap_snapshot_.load(std::memory_order_relaxed)) {
                    if (out_cancelled) *out_cancelled = true;
                    return json::object({});
                }
                ids.push_back(Value(static_cast<int64_t>(root.object_ids[i])));
            }

            if (root.name == "Globals") {
                labels.reserve(limit);
                for (std::size_t i = 0; i < limit; ++i) {
                    auto oid = root.object_ids[i];
                    std::string label;
                    auto slot_it = slot_by_oid.find(oid);
                    if (slot_it != slot_by_oid.end()) {
                        const uint32_t slot = slot_it->second;
                        auto it = names.find(slot);
                        label = (it != names.end()) ? it->second
                                                    : "global[" + std::to_string(slot) + "]";
                    }
                    if (label.empty()) label = "Object #" + std::to_string(oid);
                    labels.push_back(Value(std::move(label)));
                }
            }

            Object root_obj{
                {"name",      Value(root.name)},
                {"objectIds", Value(std::move(ids))},
            };
            if (!labels.empty()) {
                root_obj.insert_or_assign("labels", Value(std::move(labels)));
            }
            if (root_truncated) {
                root_obj.insert_or_assign("truncated", Value(true));
                root_obj.insert_or_assign("totalCount", Value(static_cast<int64_t>(total)));
            }
            roots_arr.push_back(Value(std::move(root_obj)));
        }
    }

    /// Cons pool statistics
    auto pool = heap.cons_pool().stats();
    auto cons_pool_obj = json::object({
        {"capacity", Value(static_cast<int64_t>(pool.capacity))},
        {"live",     Value(static_cast<int64_t>(pool.live_count))},
        {"free",     Value(static_cast<int64_t>(pool.free_count))},
        {"bytes",    Value(static_cast<int64_t>(pool.bytes))},
    });
    const int64_t kinds_shown = static_cast<int64_t>(kinds_arr.size());

    return json::object({
        {"totalBytes",      Value(static_cast<int64_t>(heap.total_bytes()))},
        {"softLimit",       Value(static_cast<int64_t>(heap.soft_limit()))},
        {"kinds",           Value(std::move(kinds_arr))},
        {"nativeObjectTypes", Value(std::move(native_types_arr))},
        {"roots",           Value(std::move(roots_arr))},
        {"consPool",        Value(std::move(cons_pool_obj))},
        {"truncated",       Value(truncated)},
        {"scannedObjects",  Value(scanned_objects)},
        {"kindsTotal",      Value(kinds_total_rows)},
        {"kindsShown",      Value(kinds_shown)},
        {"kindsTruncated",  Value(kinds_truncated)},
    });
}

/**
 */

void DapServer::handle_inspect_object(const Value& id, const Value& args) {
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (!driver_) {
        send_error_response(id, 2001, "VM not running");
        return;
    }
    if (!driver_->vm().is_paused()) {
        send_error_response(id, 2002, "VM must be paused to inspect the heap");
        return;
    }

    auto oid_opt = args.get_int("objectId");
    if (!oid_opt) {
        send_error_response(id, 2003, "Missing objectId argument");
        return;
    }
    auto object_id = static_cast<runtime::memory::heap::ObjectId>(*oid_opt);

    auto& heap = driver_->heap();
    runtime::memory::heap::HeapEntry entry;
    if (!heap.try_get(object_id, entry)) {
        send_error_response(id, 2004, "Object not found");
        return;
    }

    /// Format a human-readable preview using the value formatter
    auto lisp_val = runtime::nanbox::ops::box(runtime::nanbox::Tag::HeapObject, object_id);
    std::string preview = driver_->format_value(lisp_val);
    const auto native_info = runtime::memory::heap::native_object_inspection_info(entry);
    if (native_info.has_value() && !native_info->display.empty()) {
        if (!preview.empty()) preview.append(" ");
        preview.append(native_info->display);
    }
    const auto kind_label = runtime::memory::heap::heap_entry_kind_label(entry);

    /// Collect child references via the centralized heap visitor
    Array children;
    constexpr int MAX_CHILDREN = 50;

    runtime::memory::gc::visit_heap_refs(entry, [&](runtime::nanbox::LispVal child) {
        if (static_cast<int>(children.size()) >= MAX_CHILDREN) return;
        if (!runtime::nanbox::ops::is_boxed(child) || runtime::nanbox::ops::tag(child) != runtime::nanbox::Tag::HeapObject)
            return;

        auto child_id = runtime::nanbox::ops::payload(child);
        runtime::memory::heap::HeapEntry child_entry;
        if (!heap.try_get(static_cast<runtime::memory::heap::ObjectId>(child_id), child_entry))
            return;

        auto child_val = runtime::nanbox::ops::box(
            runtime::nanbox::Tag::HeapObject,
            static_cast<runtime::memory::heap::ObjectId>(child_id));
        auto child_preview = driver_->format_value(child_val);
        const auto child_native_info = runtime::memory::heap::native_object_inspection_info(child_entry);
        if (child_native_info.has_value() && !child_native_info->display.empty()) {
            if (!child_preview.empty()) child_preview.append(" ");
            child_preview.append(child_native_info->display);
        }
        Object child_obj{
            {"objectId", Value(static_cast<int64_t>(child_id))},
            {"kind",     Value(runtime::memory::heap::heap_entry_kind_label(child_entry))},
            {"size",     Value(static_cast<int64_t>(child_entry.size))},
            {"preview",  Value(std::move(child_preview))},
        };
        if (child_native_info.has_value()) {
            if (!child_native_info->type_name.empty()) {
                child_obj.insert_or_assign("nativeTypeName", Value(child_native_info->type_name));
            }
            if (!child_native_info->display.empty()) {
                child_obj.insert_or_assign("nativeDisplay", Value(child_native_info->display));
            }
        }
        children.push_back(Value(std::move(child_obj)));
    });

    Object response{
        {"objectId", Value(static_cast<int64_t>(object_id))},
        {"kind",     Value(kind_label)},
        {"size",     Value(static_cast<int64_t>(entry.size))},
        {"preview",  Value(std::move(preview))},
        {"children", Value(std::move(children))},
    };
    if (native_info.has_value()) {
        if (!native_info->type_name.empty()) {
            response.insert_or_assign("nativeTypeName", Value(native_info->type_name));
        }
        if (!native_info->display.empty()) {
            response.insert_or_assign("nativeDisplay", Value(native_info->display));
        }
    }
    send_response(id, Value(std::move(response)));
}

/**
 * disassemble (standard DAP request)
 */

void DapServer::handle_standard_disassemble(const Value& id, const Value& args) {
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (!driver_) {
        send_error_response(id, 2001, "VM not running");
        return;
    }
    if (!driver_->vm().is_paused()) {
        send_error_response(id, 2002, "VM must be paused to disassemble");
        return;
    }

    const int64_t instruction_offset = args.get_int("instructionOffset").value_or(0);
    const int64_t instruction_count = args.get_int("instructionCount").value_or(64);

    bool disassemble_all = false;
    if (auto mr = args.get_string("memoryReference")) {
        if (*mr == "all") disassemble_all = true;
    }

    runtime::vm::Disassembler disasm(driver_->heap(), driver_->intern_table());
    std::ostringstream oss;
    const runtime::vm::BytecodeFunction* current_func = nullptr;

    if (disassemble_all) {
        disasm.disassemble_all(driver_->registry(), oss);
    } else {
        auto frames = driver_->vm().get_frames();
        if (!frames.empty()) {
            const auto& top = frames[0];
            for (const auto& func : driver_->registry().all()) {
                if (func.name == top.func_name ||
                    (!top.func_name.empty() && func.name.find(top.func_name) != std::string::npos)) {
                    current_func = &func;
                    disasm.disassemble(func, oss);
                    break;
                }
            }
        }
        if (!current_func) {
            disasm.disassemble_all(driver_->registry(), oss);
        }
    }

    struct ParsedLine {
        int64_t address{0};
        std::string instruction;
    };

    std::vector<ParsedLine> parsed;
    std::istringstream lines(oss.str());
    std::string line;
    std::regex instruction_re(R"(^\s*(\d+)\s*:\s*(.*)$)");
    while (std::getline(lines, line)) {
        std::smatch m;
        if (!std::regex_match(line, m, instruction_re)) continue;

        int64_t addr = 0;
        if (!try_parse_i64(m[1].str(), addr)) continue;

        ParsedLine pl;
        pl.address = addr;
        pl.instruction = m[2].str();
        parsed.push_back(std::move(pl));
    }

    std::size_t begin = 0;
    if (instruction_offset > 0) {
        begin = static_cast<std::size_t>(instruction_offset);
        if (begin > parsed.size()) begin = parsed.size();
    }
    std::size_t end = parsed.size();
    if (instruction_count >= 0) {
        const std::size_t count = static_cast<std::size_t>(instruction_count);
        end = (std::min)(parsed.size(), begin + count);
    }

    Array instructions;
    for (std::size_t i = begin; i < end; ++i) {
        Object entry{
            {"address", Value(std::to_string(parsed[i].address))},
            {"instruction", Value(parsed[i].instruction)},
        };

        if (current_func && parsed[i].address >= 0 &&
            static_cast<std::size_t>(parsed[i].address) < current_func->source_map.size()) {
            const auto sp = current_func->source_map[static_cast<std::size_t>(parsed[i].address)];
            if (sp.file_id != 0 && sp.start.line != 0) {
                entry["line"] = Value(static_cast<int64_t>(sp.start.line));
                if (const auto* p = driver_->path_for_file_id(sp.file_id)) {
                    entry["location"] = json::object({
                        {"name", p->filename().string()},
                        {"path", p->string()},
                    });
                }
            }
        }

        instructions.push_back(Value(std::move(entry)));
    }

    send_response(id, json::object({
        {"instructions", Value(std::move(instructions))},
    }));
}

/**
 */

void DapServer::handle_disassemble(const Value& id, const Value& args) {
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (!driver_) {
        send_error_response(id, 2001, "VM not running");
        return;
    }
    if (!driver_->vm().is_paused()) {
        send_error_response(id, 2002, "VM must be paused to disassemble");
        return;
    }

    auto scope_opt = args.get_string("scope");
    std::string scope = scope_opt ? *scope_opt : "current";

    runtime::vm::Disassembler disasm(driver_->heap(), driver_->intern_table());
    std::ostringstream oss;

    std::string function_name;
    int64_t current_pc = driver_->vm().paused_instruction_index();
    bool rendered = false;

    if (scope == "all") {
        /// Disassemble all functions in the registry
        disasm.disassemble_all(driver_->registry(), oss);
        rendered = true;
    } else {
        /// Disassemble the current frame's function
        auto frames = driver_->vm().get_frames();
        if (!frames.empty()) {
            const auto& top = frames[0];
            function_name = top.func_name;

            /// Resolve the frame function without falling back to "all":
            /// the full-registry disassembly can be very expensive and can
            /// stall debugger responsiveness when refreshed on each stop.
            const runtime::vm::BytecodeFunction* target = nullptr;
            const auto& funcs = driver_->registry().all();

            for (const auto& func : funcs) {
                if (func.name == top.func_name) {
                    target = &func;
                    break;
                }
            }
            if (!target && !top.func_name.empty()) {
                const std::string suffix = "." + top.func_name;
                for (const auto& func : funcs) {
                    if (func.name.ends_with(suffix)) {
                        target = &func;
                        break;
                    }
                }
            }
            if (!target && !top.func_name.empty()) {
                for (const auto& func : funcs) {
                    if (func.name.find(top.func_name) != std::string::npos) {
                        target = &func;
                        break;
                    }
                }
            }

            if (target) {
                disasm.disassemble(*target, oss);
                rendered = true;
            }
        }
    }

    if (!rendered) {
        if (!function_name.empty()) {
            oss << "; Current function not found in registry: " << function_name << "\n";
        } else {
            oss << "; No active frame to disassemble.\n";
        }
    }

    send_response(id, json::object({
        {"text",         Value(oss.str())},
        {"functionName", Value(function_name)},
        {"currentPC",    Value(current_pc)},
    }));
}
} ///< namespace eta::dap

