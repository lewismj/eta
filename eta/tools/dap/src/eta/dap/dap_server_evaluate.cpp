/// DAP evaluate, completions and symbol docs in the paused frame.
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
 * evaluate
 */

void DapServer::handle_evaluate(const Value& id, const Value& args) {
    auto expr_opt = args.get_string("expression");
    if (!expr_opt) {
        send_response(id, json::object({{"result", "<not available>"}, {"variablesReference", 0}}));
        return;
    }

    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (!driver_) {
        send_response(id, json::object({{"result", "<not available>"}, {"variablesReference", 0}}));
        return;
    }

    /// Determine target thread from the optional frameId (carries threadId in
    /// its high bits).  No frameId -> main thread.
    int thread_id = MAIN_THREAD_ID;
    if (auto fid = args.get_int("frameId")) {
        const int t = (static_cast<int>(*fid) >> 16) & 0xFFF;
        if (t != 0) thread_id = t;
    }
    DapThread* th = find_thread_locked(thread_id);
    if (!th) th = find_thread_locked(MAIN_THREAD_ID);
    if (!th || !th->driver || !th->vm) {
        send_response(id, json::object({{"result", "<not available>"}, {"variablesReference", 0}}));
        return;
    }
    session::Driver& drv = *th->driver;
    runtime::vm::VM& vm = *th->vm;

    /**
     * When the VM is paused mid-execution, calling run_source() would invoke
     * vm_.execute() on the live stack/frame state, corrupting it and breaking
     * subsequent stepping.  Instead, do a safe name lookup in the current frames.
     */
    if (vm.is_paused()) {
        const std::string expr = trim_copy(*expr_opt);
        uint64_t value = runtime::nanbox::Nil;
        if (is_identifier_expr(expr) && try_lookup_paused_name(drv, expr, value)) {
            int vr = is_compound_value(value) ? alloc_compound_ref(value, thread_id) : 0;
            send_response(id, json::object({
                {"result",             Value(drv.format_value(value))},
                {"variablesReference", Value(static_cast<int64_t>(vr))},
            }));
            return;
        }

        std::string formatted;
        uint64_t    sandbox_val = runtime::nanbox::Nil;
        std::string sandbox_err;
        bool        violation = false;
        if (eval_in_paused_frame(drv, 0, expr, formatted, &sandbox_val, &sandbox_err, &violation)) {
            int vr = is_compound_value(sandbox_val) ? alloc_compound_ref(sandbox_val, thread_id) : 0;
            send_response(id, json::object({
                {"result",             Value(formatted)},
                {"variablesReference", Value(static_cast<int64_t>(vr))},
            }));
            return;
        }

        const std::string label = violation ? "<sandbox blocked: " : "<eval error: ";
        send_response(id, json::object({
            {"result",             Value(label + sandbox_err + ">")},
            {"variablesReference", 0},
        }));
        return;
    }

    /// Not paused: only safe to evaluate against the parent (main) driver,
    /// since run_source compiles into the parent's registry/globals.  Child
    /// VMs cannot safely accept arbitrary new source while suspended.
    runtime::nanbox::LispVal result{};
    bool ok = driver_->run_source(*expr_opt, &result);
    std::string val_str = ok ? driver_->format_value(result) : "<eval error>";

    send_response(id, json::object({
        {"result",             Value(val_str)},
        {"variablesReference", 0},
    }));
}

/**
 * Helpers
 */

bool DapServer::is_identifier_expr(const std::string& expr) {
    if (expr.empty()) return false;
    const auto is_ident_char = [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '?' || c == '!' || c == '.';
    };
    for (const char c : expr) {
        if (!is_ident_char(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool DapServer::try_lookup_paused_name(session::Driver& drv, const std::string& expr, uint64_t& out_val) {
    auto frames = drv.vm().get_frames();

    /// 1. Search locals and upvalues across all frames.
    for (std::size_t fi = 0; fi < frames.size(); ++fi) {
        for (const auto& e : drv.vm().get_locals(fi)) {
            if (e.name == expr) {
                out_val = e.value;
                return true;
            }
        }
        for (const auto& e : drv.vm().get_upvalues(fi)) {
            if (e.name == expr) {
                out_val = e.value;
                return true;
            }
        }
    }

    /// 2. Search globals by exact full name first.
    const auto& gvals  = drv.vm().globals();
    const auto& gnames = drv.global_names();
    for (const auto& [slot, full_name] : gnames) {
        if (full_name == expr && slot < gvals.size()) {
            out_val = gvals[slot];
            return true;
        }
    }

    /// 3. Search globals by short name.
    for (const auto& [slot, full_name] : gnames) {
        const auto dot = full_name.rfind('.');
        if (dot == std::string::npos) continue;
        if (full_name.substr(dot + 1) == expr && slot < gvals.size()) {
            out_val = gvals[slot];
            return true;
        }
    }

    return false;
}

bool DapServer::eval_in_paused_frame(session::Driver& drv,
                                     int /*frame_idx*/,
                                     const std::string& expr,
                                     std::string& out_str,
                                     uint64_t* out_val,
                                     std::string* out_error,
                                     bool* out_violation) {
    out_str.clear();
    if (out_error)     out_error->clear();
    if (out_violation) *out_violation = false;

    if (!drv.vm().is_paused()) {
        if (out_error) *out_error = "VM must be paused";
        return false;
    }

    runtime::vm::Sandbox sandbox(
        drv.heap(),
        drv.intern_table(),
        [this, &drv](const std::string& name, runtime::nanbox::LispVal& v) -> bool {
            return try_lookup_paused_name(drv, name, v);
        });

    auto sandbox_result = [&] {
        runtime::vm::VM::SandboxScope sandbox_scope(drv.vm());
        return sandbox.eval(expr);
    }();

    if (!sandbox_result.ok()) {
        if (out_error)     *out_error     = sandbox_result.error;
        if (out_violation) *out_violation = sandbox_result.violation;
        return false;
    }

    if (out_val) *out_val = sandbox_result.value;
    out_str = drv.format_value(sandbox_result.value);
    return true;
}

/**
 */
void DapServer::handle_symbol_doc(const Value& id, const Value& args) {
    const auto symbol = args.get_string("symbol");
    if (!symbol || symbol->empty()) {
        send_response(id, json::object({
            {"found", false},
        }));
        return;
    }

    if (auto entry = eta::reader::lookup_special_form_doc(*symbol)) {
        send_response(id, json::object({
            {"found", true},
            {"name", std::string(entry->name)},
            {"symbolKind", "special-form"},
            {"kind", "markdown"},
            {"category", std::string(entry->category)},
            {"signature", std::string(entry->signature)},
            {"documentation", eta::docs::render_markdown(*entry)},
        }));
        return;
    }

    if (auto builtin = eta::runtime::lookup_builtin_metadata(*symbol)) {
        send_response(id, json::object({
            {"found", true},
            {"name", builtin->name},
            {"symbolKind", "builtin"},
            {"kind", "markdown"},
            {"category", builtin->category},
            {"signature", eta::runtime::format_builtin_signature(*builtin)},
            {"documentation", eta::docs::render_builtin_markdown(*builtin)},
        }));
        return;
    }

    if (auto stdlib_doc = eta::docs::lookup_stdlib_doc(*symbol)) {
        send_response(id, json::object({
            {"found", true},
            {"name", std::string(stdlib_doc->name)},
            {"symbolKind", "stdlib"},
            {"kind", "markdown"},
            {"category", std::string(stdlib_doc->category)},
            {"module", std::string(stdlib_doc->module)},
            {"signature", std::string(stdlib_doc->signature)},
            {"documentation", eta::docs::render_markdown(*stdlib_doc)},
        }));
        return;
    }

    send_response(id, json::object({
        {"found", false},
    }));
}

/**
 */

void DapServer::handle_completions(const Value& id, const Value& args) {
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (!driver_ || !driver_->vm().is_paused()) {
        send_response(id, json::object({{"targets", json::array({})}}));
        return;
    }

    auto text_opt = args.get_string("text");
    std::string prefix = text_opt ? *text_opt : "";
    /// Strip leading whitespace and opening parens
    while (!prefix.empty() && (prefix.front() == '(' || prefix.front() == ' '))
        prefix.erase(prefix.begin());

    Array targets;
    std::unordered_set<std::string> seen;

    /// Collect from all frames: locals + upvalues
    auto frames = driver_->vm().get_frames();
    for (std::size_t fi = 0; fi < frames.size(); ++fi) {
        for (const auto& e : driver_->vm().get_locals(fi)) {
            if (!e.name.empty() && e.name[0] != '%' && seen.insert(e.name).second) {
                if (prefix.empty() || e.name.find(prefix) == 0) {
                    targets.push_back(json::object({
                        {"label", Value(e.name)},
                        {"type",  "variable"},
                    }));
                }
            }
        }
        for (const auto& e : driver_->vm().get_upvalues(fi)) {
            if (!e.name.empty() && e.name[0] != '&' && seen.insert(e.name).second) {
                if (prefix.empty() || e.name.find(prefix) == 0) {
                    targets.push_back(json::object({
                        {"label", Value(e.name)},
                        {"type",  "variable"},
                    }));
                }
            }
        }
    }

    send_response(id, json::object({{"targets", Value(std::move(targets))}}));
}

/**
 * current_module_from_frame
 */

std::string DapServer::current_module_from_frame(session::Driver& drv, std::size_t frame_idx) {
    auto frames = drv.vm().get_frames();
    if (frame_idx >= frames.size()) return "";
    const std::string& fn = frames[frame_idx].func_name;

    const auto& names = drv.global_names();
    std::string best_mod;
    std::size_t best_len = 0;

    for (const auto& [slot, full_name] : names) {
        auto dot = full_name.rfind('.');
        if (dot == std::string::npos) continue;
        std::string mod = full_name.substr(0, dot);
        if (mod.size() <= best_len) continue;

        std::string mod_ul = mod;
        for (char& c : mod_ul) if (c == '.') c = '_';

        if (fn.size() >= mod_ul.size() &&
            fn.compare(0, mod_ul.size(), mod_ul) == 0 &&
            (fn.size() == mod_ul.size() || fn[mod_ul.size()] == '_')) {
            best_mod = mod;
            best_len = mod_ul.size();
        }
    }
    return best_mod;
}
} ///< namespace eta::dap

