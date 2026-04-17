#include "dap_server.h"
#include "dap_io.h"

#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>

/// Eta interpreter
#include "eta/interpreter/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/runtime/port.h"
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/vm/disassembler.h"
#include "eta/runtime/value_formatter.h"
#include "eta/runtime/memory/mark_sweep_gc.h"
#include "eta/runtime/memory/cons_pool.h"
#include "eta/runtime/types/types.h"
#include "eta/diagnostic/diagnostic.h"

#ifdef ETA_HAS_NNG
#include "eta/nng/nng_socket_ptr.h"
#include "eta/nng/nng_primitives.h"
#endif

namespace eta::dap {

namespace fs = std::filesystem;
using namespace eta::json;

/**
 * Local helpers
 */

/**
 * Normalise a source-file path to a stable key that matches the normalisation
 * used by Driver::file_id_for_path / Driver::ensure_file_id.
 */
static std::string normalize_path(const std::string& raw) {
    std::error_code ec;
    fs::path abs = fs::absolute(fs::path(raw), ec);
    if (ec) abs = fs::path(raw);
    std::string s = abs.lexically_normal().string();
#ifdef _WIN32
    for (char& c : s) {
        if (c == '/') c = '\\';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
#endif
    return s;
}

/**
 * Construction
 */

DapServer::DapServer() : in_(std::cin), out_(std::cout) {}

DapServer::DapServer(std::istream& in, std::ostream& out) : in_(in), out_(out) {}

/**
 * Destruction
 */

DapServer::~DapServer() {
    /**
     * If the VM thread is still running, wake it (in case it's blocked on
     * debug_cv_ after a pause) so the thread can finish and be joined.
     * debug_cv_ for resume(), not for another pause request.
     */
    if (driver_) {
        driver_->vm().resume();
    }
    if (vm_thread_.joinable()) {
        vm_thread_.join();
    }
}

/**
 * Transport
 */

void DapServer::send(const Value& msg) {
    std::lock_guard<std::mutex> lk(output_mutex_);
    write_message(out_, json::to_string(msg));
}

void DapServer::send_response(const Value& id, const Value& body) {
    send(json::object({
        {"seq",         Value(next_seq_++)},
        {"type",        "response"},
        {"command",     Value(current_command_)},
        {"request_seq", id},
        {"success",     true},
        {"body",        body},
    }));
}

void DapServer::send_error_response(const Value& id, int code, const std::string& msg) {
    send(json::object({
        {"seq",         Value(next_seq_++)},
        {"type",        "response"},
        {"command",     Value(current_command_)},
        {"request_seq", id},
        {"success",     false},
        {"body",        json::object({
            {"error", json::object({
                {"id",     Value(static_cast<int64_t>(code))},
                {"format", Value(msg)},
            })},
        })},
    }));
}

void DapServer::send_event(const std::string& event_name, const Value& body) {
    send(json::object({
        {"seq",   Value(next_seq_++)},
        {"type",  "event"},
        {"event", event_name},
        {"body",  body},
    }));
}

/**
 * Main loop
 */

void DapServer::run() {
    while (running_) {
        auto msg_str = read_message(in_);
        if (!msg_str) break;

        try {
            auto msg = json::parse(*msg_str);
            dispatch(msg);
        } catch (const std::exception& e) {
            std::cerr << "[eta_dap] parse error: " << e.what() << "\n";
        }
    }

    /// Join the VM thread if still running
    if (vm_thread_.joinable()) {
        vm_thread_.join();
    }
}

/**
 * Dispatch
 */

void DapServer::dispatch(const Value& msg) {
    if (!msg.is_object()) return;

    auto cmd = msg.get_string("command");
    if (!cmd) return;

    const Value& id   = msg["seq"];
    const Value& args = msg.has("arguments") ? msg["arguments"] : Value{};
    current_command_  = *cmd;

    if (*cmd == "initialize")               handle_initialize(id, args);
    else if (*cmd == "launch")              handle_launch(id, args);
    else if (*cmd == "setBreakpoints")      handle_set_breakpoints(id, args);
    else if (*cmd == "setExceptionBreakpoints") handle_set_exception_breakpoints(id, args);
    else if (*cmd == "configurationDone")   handle_configuration_done(id, args);
    else if (*cmd == "threads")             handle_threads(id, args);
    else if (*cmd == "stackTrace")          handle_stack_trace(id, args);
    else if (*cmd == "scopes")              handle_scopes(id, args);
    else if (*cmd == "variables")           handle_variables(id, args);
    else if (*cmd == "continue")            handle_continue(id, args);
    else if (*cmd == "next")                handle_next(id, args);
    else if (*cmd == "stepIn")              handle_step_in(id, args);
    else if (*cmd == "stepOut")             handle_step_out(id, args);
    else if (*cmd == "pause")               handle_pause(id, args);
    else if (*cmd == "evaluate")            handle_evaluate(id, args);
    else if (*cmd == "terminate")           handle_terminate(id, args);
    else if (*cmd == "completions")         handle_completions(id, args);
    else if (*cmd == "disconnect")          handle_disconnect(id, args);
    else if (*cmd == "eta/heapSnapshot")    handle_heap_inspector(id, args);
    else if (*cmd == "eta/inspectObject")   handle_inspect_object(id, args);
    else if (*cmd == "eta/disassemble")     handle_disassemble(id, args);
    else if (*cmd == "eta/childProcesses")  handle_child_processes(id, args);
    else {
        send_response(id, json::object({}));
    }
}

/**
 * initialize
 */

void DapServer::handle_initialize(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({
        {"supportsConfigurationDoneRequest",    true},
        /**
         * supportsSetBreakpoints is not an official DAP capability field (all
         * adapters support setBreakpoints by default), but some host versions
         * check for it so we advertise it explicitly for maximum compatibility.
         */
        {"supportsSetBreakpoints",              true},
        {"supportsFunctionBreakpoints",         false},
        {"supportsConditionalBreakpoints",      false},
        {"supportsSetVariable",                 false},
        {"supportsRestartRequest",              false},
        {"supportsTerminateRequest",            true},
        {"supportsEvaluateForHovers",           true},
        {"supportsStepBack",                    false},
        {"supportsGotoTargetsRequest",          false},
        {"supportsBreakpointLocationsRequest",  false},
        {"supportsCompletionsRequest",          true},
    }));
    /**
     * IMPORTANT: "initialized" is intentionally NOT sent here.
     * This adapter uses the "deferred-initialization" DAP flow:
     * If we sent "initialized" here instead, VS Code would fire setBreakpoints
     * and configurationDone BEFORE "launch" arrives.  handle_configuration_done
     * guards on (launched_ == true), so it would return immediately, the VM
     * would never start, and every breakpoint would be silently lost.
     * See handle_launch() for where "initialized" is actually sent.
     */
}

/**
 * launch
 */

void DapServer::handle_launch(const Value& id, const Value& args) {
    auto program = args.get_string("program");
    if (!program) {
        send_error_response(id, 1001, "launch: 'program' argument is required");
        return;
    }

    script_path_    = fs::path(*program);
    stop_on_entry_  = args.has("stopOnEntry") && args["stopOnEntry"].is_bool()
                      && args["stopOnEntry"].as_bool();
    launched_       = true;

    send_response(id, json::object({}));

    /**
     * Per DAP spec: send "initialized" AFTER the launch response so VS Code
     * sends setBreakpoints and configurationDone AFTER script_path_ is set.
     */
    send_event("initialized", json::object({}));

    send_event("output", json::object({
        {"category", "console"},
        {"output", "[eta_dap] Launch: " + script_path_.string() + "\n"},
    }));
    /// The VM is actually started on configurationDone.
}

/**
 * setBreakpoints
 */

void DapServer::handle_set_breakpoints(const Value& id, const Value& args) {
    Array result_bps;

    auto source_obj  = args["source"];
    auto path_val    = source_obj.get_string("path");
    if (!path_val) {
        send_response(id, json::object({{"breakpoints", json::array({})}}));
        return;
    }

    /// Normalise so it matches the driver's canon_path_key lookup
    std::string canon_path = normalize_path(*path_val);
    pending_bps_.erase(canon_path);

    if (args.has("breakpoints") && args["breakpoints"].is_array()) {
        for (const auto& bp : args["breakpoints"].as_array()) {
            auto line_opt = bp.get_int("line");
            if (!line_opt) continue;
            int line  = static_cast<int>(*line_opt);
            int bp_id = next_bp_id_++;

            pending_bps_[canon_path].push_back({line, bp_id});

            result_bps.push_back(json::object({
                {"verified", false},   ///< updated via "breakpoint" event once installed
                {"id",       Value(static_cast<int64_t>(bp_id))},
                {"line",     Value(static_cast<int64_t>(line))},
            }));
        }
    }

    /// Diagnostic: always log what was received so path mismatches are visible
    {
        std::ostringstream msg;
        msg << "[eta_dap] setBreakpoints: " << result_bps.size()
            << " bp(s) for \"" << canon_path << "\"\n";
        send_event("output", json::object({{"category", "console"}, {"output", msg.str()}}));
    }

    /// If the VM is already running, push immediately and notify VS Code
    {
        std::lock_guard<std::mutex> lk(vm_mutex_);
        if (driver_) {
            install_pending_breakpoints();
        }
    }
    /// Check if we had a driver to decide whether to notify (no lock needed for reads)
    if (driver_) {
        notify_breakpoints_verified();
        /// Mark returned breakpoints as verified in the response too
        for (auto& bp_val : result_bps) {
            if (bp_val.is_object()) bp_val.as_object()["verified"] = Value(true);
        }
    }

    send_response(id, json::object({{"breakpoints", Value(std::move(result_bps))}}));
}

/**
 * setExceptionBreakpoints
 */

void DapServer::handle_set_exception_breakpoints(const Value& id, const Value& args) {
    /// Parse the filters array to determine whether exception breakpoints are enabled.
    exception_breakpoints_enabled_ = false;
    if (args.is_object()) {
        const auto& filters_val = args["filters"];
        if (filters_val.is_array()) {
            for (const auto& f : filters_val.as_array()) {
                if (f.is_string() && !f.as_string().empty()) {
                    exception_breakpoints_enabled_ = true;
                    break;
                }
            }
        }
    }
    send_response(id, json::object({}));
}

/**
 */

void DapServer::handle_configuration_done(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({}));

    if (!launched_) return; ///< no launch request yet (shouldn't happen)

    /// Build the module search path using ModulePathResolver (same logic as etai / eta_lsp)
    auto resolver = interpreter::ModulePathResolver::from_args_or_env("");
    /// Also search the directory containing the script being debugged.
    auto script_dir = script_path_.parent_path();
    if (!script_dir.empty()) resolver.add_dir(script_dir);

    /// Log the module search path so the user can diagnose "module not found"
    {
        std::ostringstream msg;
        msg << "[eta_dap] Module search dirs:\n";
        if (resolver.empty()) {
            msg << "  (none â€” prelude will not load; set eta.lsp.modulePath in VS Code settings)\n";
        } else {
            for (const auto& d : resolver.dirs()) msg << "  " << d.string() << "\n";
        }
        send_event("output", json::object({{"category", "console"}, {"output", msg.str()}}));
    }


    /// Build the driver on the DAP thread (it will be moved-to below)
    auto drv = std::make_unique<interpreter::Driver>(std::move(resolver));

    /// Register stop callback BEFORE loading prelude so the hook is in place
    drv->vm().set_stop_callback([this](const runtime::vm::StopEvent& ev) {
        using runtime::vm::StopReason;

        /// Clear compound variable refs from the previous stop
        clear_compound_refs();

        std::string reason_str;
        switch (ev.reason) {
            case StopReason::Breakpoint: reason_str = "breakpoint"; break;
            case StopReason::Step:       reason_str = "step";       break;
            case StopReason::Pause:      reason_str = "pause";      break;
            case StopReason::Exception:  reason_str = "exception";  break;
            default:                     reason_str = "pause";      break;
        }

        send_event("stopped", json::object({
            {"reason",            reason_str},
            {"threadId",          1},
            {"allThreadsStopped", true},
        }));
    });

    /**
     * Redirect script stdout/stderr away from the protocol pipe
     * Do this on the LOCAL drv BEFORE making it visible via driver_.
     * Without this, any script that calls (display ...) or (newline) would write
     * to std::cout, corrupting the Content-Length framed DAP stream and causing
     * VS Code to report a "read error".  We install CallbackPorts that forward
     * the text as custom "eta-output" DAP events.  The VS Code extension's
     * DebugAdapterTracker intercepts these and writes to a dedicated "Eta Output"
     * OutputChannel, so script output appears in the Output panel rather than the
     * Debug Console.
     */
    drv->set_output_port(std::make_shared<runtime::CallbackPort>(
        [this](const std::string& text) {
            send_event("eta-output", json::object({{"stream", "stdout"}, {"text", text}}));
        }));
    drv->set_error_port(std::make_shared<runtime::CallbackPort>(
        [this](const std::string& text) {
            send_event("eta-output", json::object({{"stream", "stderr"}, {"text", text}}));
        }));

    /**
     * Pre-register the script file ID so breakpoints fire on first run
     * We allocate the file_id NOW (before the VM thread starts loading any file).
     * When run_file() is later called with the same (normalised) path, Driver
     * inside vm_.execute().
     */
    drv->ensure_file_id(script_path_);

    /**
     * Atomically publish driver_ and install breakpoints under the lock
     * All setup is done on the local drv above; only now do we make it visible
     * to other DAP-thread handlers (evaluate, setBreakpoints, etc.).
     */
    {
        std::lock_guard<std::mutex> lk(vm_mutex_);
        driver_ = std::move(drv);
        install_pending_breakpoints();
    }
    /**
     * Send "breakpoint" changed events for all newly-verified breakpoints.
     * Must be called WITHOUT vm_mutex_ held (uses output_mutex_ internally).
     */
    notify_breakpoints_verified();

    {
        std::size_t bp_count = 0;
        for (const auto& [p, bps] : pending_bps_) bp_count += bps.size();
        std::ostringstream msg;
        msg << "[eta_dap] " << bp_count << " breakpoint(s) installed; "
            << "script file_id = " << driver_->file_id_for_path(script_path_.string()) << "\n";
        send_event("output", json::object({{"category", "console"}, {"output", msg.str()}}));
    }

    /// If stopOnEntry, request a pause immediately (before any code runs)
    if (stop_on_entry_) {
        driver_->vm().request_pause();
    }

    /// Launch the VM on a background thread
    vm_thread_ = std::thread([this]() {
        auto* drv = driver_.get();

        /// Load prelude
        auto pr = drv->load_prelude();
        if (!pr.found) {
            send_event("output", json::object({
                {"category", "important"},
                {"output",
                    "[eta_dap] Warning: prelude.eta not found â€” std.core, std.io, std.prelude etc. unavailable.\n"
                    "[eta_dap] Set 'eta.lsp.modulePath' in VS Code settings, or set ETA_MODULE_PATH.\n"},
            }));
        } else if (!pr.loaded) {
            auto resolve = drv->file_resolver();
            const auto& diags = drv->diagnostics().diagnostics();
            for (const auto& d : diags) {
                std::ostringstream msg;
                diagnostic::format_diagnostic(msg, d, /*use_color=*/false, resolve);
                send_event("output", json::object({{"category", "stderr"}, {"output", msg.str() + "\n"}}));
            }
        } else {
            send_event("output", json::object({
                {"category", "console"},
                {"output", "[eta_dap] Prelude loaded: " + pr.path.string() + "\n"},
            }));
        }

        /**
         * Re-install now that prelude file IDs are known, so breakpoints in
         * library files are active before the script starts executing.
         */
        {
            std::lock_guard<std::mutex> lk(vm_mutex_);
            install_pending_breakpoints();
        }
        /// Notify VS Code about any breakpoints that became verified after prelude load
        notify_breakpoints_verified();

        /// Execute the script
        bool ok = drv->run_file(script_path_);

        /// Signal IDE
        if (ok) {
            send_event("terminated", json::object({}));
        } else {
            /// a generic "Script failed" message.
            const auto& diags = drv->diagnostics().diagnostics();
            if (diags.empty()) {
                send_event("output", json::object({
                    {"category", "stderr"},
                    {"output", "[eta_dap] Script failed: " + script_path_.filename().string() + "\n"},
                }));
            } else {
                auto resolve = drv->file_resolver();
                for (const auto& d : diags) {
                    std::ostringstream msg;
                    diagnostic::format_diagnostic(msg, d, /*use_color=*/false, resolve);
                    send_event("output", json::object({{"category", "stderr"}, {"output", msg.str() + "\n"}}));
                }
            }
            send_event("terminated", json::object({}));
        }
    });
}

/**
 * threads
 */

void DapServer::handle_threads(const Value& id, const Value& /*args*/) {
    Array threads;
    threads.push_back(json::object({{"id", 1}, {"name", "main"}}));
#ifdef ETA_HAS_NNG
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (driver_ && driver_->process_manager()) {
        int tid = 2;
        for (const auto& ti : driver_->process_manager()->list_threads()) {
            std::string name = "actor-" + std::to_string(tid - 1);
            if (!ti.func_name.empty()) name += " (" + ti.func_name + ")";
            threads.push_back(json::object({
                {"id",   Value(static_cast<int64_t>(tid))},
                {"name", Value(name)},
            }));
            ++tid;
        }
    }
#endif
    send_response(id, json::object({{"threads", Value(std::move(threads))}}));
}

/**
 * stackTrace
 */

void DapServer::handle_stack_trace(const Value& id, const Value& /*args*/) {
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (!driver_ || !driver_->vm().is_paused()) {
        send_response(id, json::object({{"stackFrames", json::array({})}, {"totalFrames", 0}}));
        return;
    }

    auto frames = driver_->vm().get_frames();
    Array frames_arr;
    int frame_id = 0;
    for (const auto& fi : frames) {
        const auto& sp = fi.span;

        Value source_val = nullptr;
        if (sp.file_id != 0) {
            if (auto* path = driver_->path_for_file_id(sp.file_id)) {
                source_val = json::object({
                    {"name", path->filename().string()},
                    {"path", path->string()},
                });
            }
        }

        frames_arr.push_back(json::object({
            {"id",     Value(static_cast<int64_t>(frame_id))},
            {"name",   fi.func_name.empty() ? Value("<anonymous>") : Value(fi.func_name)},
            {"source", source_val},
            {"line",   Value(static_cast<int64_t>(sp.start.line))},
            {"column", Value(static_cast<int64_t>(sp.start.column))},
        }));
        ++frame_id;
    }

    send_response(id, json::object({
        {"stackFrames", Value(std::move(frames_arr))},
        {"totalFrames", Value(static_cast<int64_t>(frame_id))},
    }));
}

/**
 * scopes
 */

void DapServer::handle_scopes(const Value& id, const Value& args) {
    auto frame_id_opt = args.get_int("frameId");
    int frame_id = frame_id_opt ? static_cast<int>(*frame_id_opt) : 0;

    send_response(id, json::object({
        {"scopes", json::array({
            json::object({
                {"name",               "Module"},
                {"variablesReference", Value(static_cast<int64_t>(encode_var_ref(frame_id, 3)))},
                {"expensive",          false},
                {"presentationHint",   "locals"},
            }),
            json::object({
                {"name",               "Locals"},
                {"variablesReference", Value(static_cast<int64_t>(encode_var_ref(frame_id, 0)))},
                {"expensive",          false},
            }),
            json::object({
                {"name",               "Upvalues"},
                {"variablesReference", Value(static_cast<int64_t>(encode_var_ref(frame_id, 1)))},
                {"expensive",          false},
            }),
            json::object({
                {"name",               "Globals"},
                {"variablesReference", Value(static_cast<int64_t>(encode_var_ref(frame_id, 2)))},
                {"expensive",          true},
            }),
        })},
    }));
}

/**
 * variables
 */

void DapServer::handle_variables(const Value& id, const Value& args) {
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (!driver_ || !driver_->vm().is_paused()) {
        send_response(id, json::object({{"variables", json::array({})}}));
        return;
    }

    auto ref_opt = args.get_int("variablesReference");
    if (!ref_opt) {
        send_response(id, json::object({{"variables", json::array({})}}));
        return;
    }

    int ref = static_cast<int>(*ref_opt);

    /// Compound variable expansion (cons/vector/closure)
    if (ref >= COMPOUND_REF_BASE) {
        auto cit = compound_refs_.find(ref);
        if (cit == compound_refs_.end()) {
            send_response(id, json::object({{"variables", json::array({})}}));
            return;
        }
        auto children = expand_compound(cit->second);
        send_response(id, json::object({{"variables", Value(std::move(children))}}));
        return;
    }

    /// Frame scope variables (locals / upvalues / globals)
    int frame_idx  = decode_var_ref_frame(ref);
    int scope      = decode_var_ref_scope(ref);

    if (scope == 3) {
        /**
         * Module scope: globals that belong to the currently-executing module
         * so they are immediately visible without scrolling through all prelude/std.* entries.
         */
        std::string cur_mod = current_module_from_frame(static_cast<std::size_t>(frame_idx));
        const auto& globals = driver_->vm().globals();
        const auto& names   = driver_->global_names();
        Array vars;
        for (const auto& [slot, full_name] : names) {
            if (slot >= globals.size()) continue;
            auto v = globals[slot];
            if (v == runtime::nanbox::Nil) continue;
            auto dot = full_name.rfind('.');
            if (dot == std::string::npos) continue;
            if (full_name.substr(0, dot) != cur_mod) continue;
            vars.push_back(make_variable_json(full_name.substr(dot + 1), v));
        }
        send_response(id, json::object({{"variables", Value(std::move(vars))}}));
        return;
    }

    if (scope == 2) {
        /// Globals scope
        const auto& globals = driver_->vm().globals();
        const auto& names   = driver_->global_names();
        Array vars;
        for (std::size_t slot = 0; slot < globals.size(); ++slot) {
            auto v = globals[slot];
            if (v == runtime::nanbox::Nil) continue;

            auto it = names.find(static_cast<uint32_t>(slot));
            std::string name = (it != names.end()) ? it->second
                                                   : "global[" + std::to_string(slot) + "]";
            vars.push_back(make_variable_json(name, v));
        }
        send_response(id, json::object({{"variables", Value(std::move(vars))}}));
        return;
    }

    auto entries = (scope == 0)
                   ? driver_->vm().get_locals(static_cast<std::size_t>(frame_idx))
                   : driver_->vm().get_upvalues(static_cast<std::size_t>(frame_idx));

    Array vars;
    for (const auto& e : entries) {
        /**
         * uninitialised scratch slots from module-init functions and just clutter
         * the Variables panel.
         */
        if (!e.name.empty() && e.name[0] == '%' && e.value == runtime::nanbox::Nil)
            continue;
        vars.push_back(make_variable_json(e.name, e.value));
    }

    send_response(id, json::object({{"variables", Value(std::move(vars))}}));
}

/**
 * continue / next / stepIn / stepOut / pause
 */

void DapServer::handle_continue(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({{"allThreadsContinued", true}}));
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (driver_) driver_->vm().resume();
}

void DapServer::handle_next(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({}));
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (driver_) driver_->vm().step_over();
}

void DapServer::handle_step_in(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({}));
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (driver_) driver_->vm().step_in();
}

void DapServer::handle_step_out(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({}));
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (driver_) driver_->vm().step_out();
}

void DapServer::handle_pause(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({}));
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (driver_) driver_->vm().request_pause();
}

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

    /**
     * When the VM is paused mid-execution, calling run_source() would invoke
     * vm_.execute() on the live stack/frame state, corrupting it and breaking
     * subsequent stepping.  Instead, do a safe name lookup in the current frames.
     */
    if (driver_->vm().is_paused()) {
        const std::string& expr = *expr_opt;
        auto frames = driver_->vm().get_frames();

        /// 1. Search locals and upvalues across all frames
        for (std::size_t fi = 0; fi < frames.size(); ++fi) {
            for (const auto& e : driver_->vm().get_locals(fi)) {
                if (e.name == expr) {
                    int vr = is_compound_value(e.value) ? alloc_compound_ref(e.value) : 0;
                    send_response(id, json::object({
                        {"result",             Value(driver_->format_value(e.value))},
                        {"variablesReference", Value(static_cast<int64_t>(vr))},
                    }));
                    return;
                }
            }
            for (const auto& e : driver_->vm().get_upvalues(fi)) {
                if (e.name == expr) {
                    int vr = is_compound_value(e.value) ? alloc_compound_ref(e.value) : 0;
                    send_response(id, json::object({
                        {"result",             Value(driver_->format_value(e.value))},
                        {"variablesReference", Value(static_cast<int64_t>(vr))},
                    }));
                    return;
                }
            }
        }

        /// 2. Search globals: exact full name ("composition.top5") first
        const auto& gvals  = driver_->vm().globals();
        const auto& gnames = driver_->global_names();
        for (const auto& [slot, full_name] : gnames) {
            if (full_name == expr && slot < gvals.size()) {
                auto v = gvals[slot];
                int vr = is_compound_value(v) ? alloc_compound_ref(v) : 0;
                send_response(id, json::object({
                    {"result",             Value(driver_->format_value(v))},
                    {"variablesReference", Value(static_cast<int64_t>(vr))},
                }));
                return;
            }
        }

        /// 3. Search globals: short name ("top5" matches "composition.top5")
        for (const auto& [slot, full_name] : gnames) {
            auto dot = full_name.rfind('.');
            if (dot == std::string::npos) continue;
            if (full_name.substr(dot + 1) == expr && slot < gvals.size()) {
                auto v = gvals[slot];
                int vr = is_compound_value(v) ? alloc_compound_ref(v) : 0;
                send_response(id, json::object({
                    {"result",             Value(driver_->format_value(v))},
                    {"variablesReference", Value(static_cast<int64_t>(vr))},
                }));
                return;
            }
        }

        send_response(id, json::object({{"result", "<not available>"}, {"variablesReference", 0}}));
        return;
    }

    runtime::nanbox::LispVal result{};
    bool ok = driver_->run_source(*expr_opt, &result);
    std::string val_str = ok ? driver_->format_value(result) : "<eval error>";

    send_response(id, json::object({
        {"result",             Value(val_str)},
        {"variablesReference", 0},
    }));
}

/**
 * terminate
 */

void DapServer::handle_terminate(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({}));
    /// Resume the VM (in case it's paused) so the vm_thread can exit gracefully.
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (driver_) {
        driver_->vm().resume();
    }
}

/**
 * disconnect
 */

void DapServer::handle_disconnect(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({}));
    running_ = false;

    /**
     * Wake up the VM if it's paused so the vm_thread can exit.
     * Lock vm_mutex_ so driver_ isn't torn out mid-access.
     */
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (driver_) {
        driver_->vm().resume();
    }
}

/**
 * Helpers
 */

void DapServer::install_pending_breakpoints() {
    if (!driver_) return;

    std::vector<runtime::vm::BreakLocation> all_locs;

    for (const auto& [path, bps] : pending_bps_) {
        uint32_t file_id = driver_->file_id_for_path(path);
        if (file_id == 0) continue;

        for (const auto& bp : bps) {
            all_locs.push_back({file_id, static_cast<uint32_t>(bp.line)});
        }
    }

    driver_->vm().set_breakpoints(std::move(all_locs));
}

void DapServer::notify_breakpoints_verified() {
    /// Collect verified breakpoint data under the lock, then send events after.
    struct VerifiedBp { int id; int line; };
    std::vector<VerifiedBp> verified;

    {
        std::lock_guard<std::mutex> lk(vm_mutex_);
        if (!driver_) return;

        for (const auto& [path, bps] : pending_bps_) {
            uint32_t file_id = driver_->file_id_for_path(path);
            if (file_id == 0) continue;

            for (const auto& bp : bps) {
                verified.push_back({bp.id, bp.line});
            }
        }
    }

    /// Send "breakpoint" changed events so VS Code shows solid red dots.
    for (const auto& v : verified) {
        send_event("breakpoint", json::object({
            {"reason", "changed"},
            {"breakpoint", json::object({
                {"id",       Value(static_cast<int64_t>(v.id))},
                {"verified", true},
                {"line",     Value(static_cast<int64_t>(v.line))},
            })},
        }));
    }
}

/**
 */

void DapServer::handle_heap_inspector(const Value& id, const Value& /*args*/) {
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (!driver_) {
        send_error_response(id, 2001, "VM not running");
        return;
    }
    if (!driver_->vm().is_paused()) {
        send_error_response(id, 2002, "VM must be paused to inspect the heap");
        return;
    }
    send_response(id, build_heap_snapshot());
}

Value DapServer::build_heap_snapshot() {
    using namespace runtime::memory::heap;

    auto& heap = driver_->heap();

    /// Per-kind statistics
    struct KindStat { int64_t count{0}; int64_t bytes{0}; };
    std::unordered_map<uint8_t, KindStat> kind_stats;

    heap.for_each_entry([&](ObjectId /*id*/, HeapEntry& entry) {
        auto k = static_cast<uint8_t>(entry.header.kind);
        kind_stats[k].count++;
        kind_stats[k].bytes += static_cast<int64_t>(entry.size);
    });

    Array kinds_arr;
    for (const auto& [k, stat] : kind_stats) {
        kinds_arr.push_back(json::object({
            {"kind",  Value(std::string(to_string(static_cast<ObjectKind>(k))))},
            {"count", Value(stat.count)},
            {"bytes", Value(stat.bytes)},
        }));
    }

    /// GC roots
    auto gc_roots = driver_->vm().enumerate_gc_roots();
    Array roots_arr;
    for (const auto& root : gc_roots) {
        Array ids;
        Array labels;
        for (std::size_t i = 0; i < root.object_ids.size(); ++i) {
            ids.push_back(Value(static_cast<int64_t>(root.object_ids[i])));
        }

        /**
         * For the "Globals" root, resolve variable names from the Driver's
         * of just "Object #id".
         */
        if (root.name == "Globals") {
            auto& globals      = driver_->vm().globals();
            const auto& names  = driver_->global_names();

            for (std::size_t i = 0; i < root.object_ids.size(); ++i) {
                auto oid = root.object_ids[i];
                std::string label;
                /// Walk globals to find which slot holds this object ID.
                for (std::size_t slot = 0; slot < globals.size(); ++slot) {
                    auto v = globals[slot];
                    if (runtime::nanbox::ops::is_boxed(v) &&
                        runtime::nanbox::ops::tag(v) == runtime::nanbox::Tag::HeapObject &&
                        static_cast<runtime::memory::heap::ObjectId>(runtime::nanbox::ops::payload(v)) == oid) {
                        auto it = names.find(static_cast<uint32_t>(slot));
                        label = (it != names.end()) ? it->second
                                                    : "global[" + std::to_string(slot) + "]";
                        break;
                    }
                }
                if (label.empty()) label = "Object #" + std::to_string(oid);
                labels.push_back(Value(std::move(label)));
            }
        }

        if (!labels.empty()) {
            roots_arr.push_back(json::object({
                {"name",      Value(root.name)},
                {"objectIds", Value(std::move(ids))},
                {"labels",    Value(std::move(labels))},
            }));
        } else {
            roots_arr.push_back(json::object({
                {"name",      Value(root.name)},
                {"objectIds", Value(std::move(ids))},
            }));
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

    return json::object({
        {"totalBytes",    Value(static_cast<int64_t>(heap.total_bytes()))},
        {"softLimit",     Value(static_cast<int64_t>(heap.soft_limit()))},
        {"kinds",         Value(std::move(kinds_arr))},
        {"roots",         Value(std::move(roots_arr))},
        {"consPool",      Value(std::move(cons_pool_obj))},
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

        auto child_val = runtime::nanbox::ops::box(runtime::nanbox::Tag::HeapObject, static_cast<runtime::memory::heap::ObjectId>(child_id));
        children.push_back(json::object({
            {"objectId", Value(static_cast<int64_t>(child_id))},
            {"kind",     Value(std::string(runtime::memory::heap::to_string(child_entry.header.kind)))},
            {"size",     Value(static_cast<int64_t>(child_entry.size))},
            {"preview",  Value(driver_->format_value(child_val))},
        }));
    });

    send_response(id, json::object({
        {"objectId", Value(static_cast<int64_t>(object_id))},
        {"kind",     Value(std::string(runtime::memory::heap::to_string(entry.header.kind)))},
        {"size",     Value(static_cast<int64_t>(entry.size))},
        {"preview",  Value(std::move(preview))},
        {"children", Value(std::move(children))},
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
    int64_t current_pc = -1;

    if (scope == "all") {
        /// Disassemble all functions in the registry
        disasm.disassemble_all(driver_->registry(), oss);
    } else {
        /// Disassemble the current frame's function
        auto frames = driver_->vm().get_frames();
        if (!frames.empty()) {
            const auto& top = frames[0];
            function_name = top.func_name;

            /// Find the function in the registry by name
            bool found = false;
            for (const auto& func : driver_->registry().all()) {
                if (func.name == top.func_name ||
                    (!top.func_name.empty() && func.name.find(top.func_name) != std::string::npos)) {
                    disasm.disassemble(func, oss);
                    found = true;
                    break;
                }
            }
            if (!found) {
                /// Fall back to disassembling all
                disasm.disassemble_all(driver_->registry(), oss);
            }
        } else {
            disasm.disassemble_all(driver_->registry(), oss);
        }
    }

    send_response(id, json::object({
        {"text",         Value(oss.str())},
        {"functionName", Value(function_name)},
        {"currentPC",    Value(current_pc)},
    }));
}

/**
 * Compound variable expansion helpers
 */

int DapServer::alloc_compound_ref(uint64_t val) {
    int ref = next_compound_ref_++;
    compound_refs_[ref] = val;
    return ref;
}

void DapServer::clear_compound_refs() {
    compound_refs_.clear();
    next_compound_ref_ = COMPOUND_REF_BASE;
}

bool DapServer::is_compound_value(uint64_t val) const {
    using namespace runtime::nanbox;
    if (!ops::is_boxed(val) || ops::tag(val) != Tag::HeapObject) return false;
    auto& heap = driver_->heap();
    runtime::memory::heap::HeapEntry entry;
    if (!heap.try_get(static_cast<runtime::memory::heap::ObjectId>(ops::payload(val)), entry))
        return false;
    switch (entry.header.kind) {
        case runtime::memory::heap::ObjectKind::Cons:
        case runtime::memory::heap::ObjectKind::Vector:
        case runtime::memory::heap::ObjectKind::Closure:
#ifdef ETA_HAS_NNG
        case runtime::memory::heap::ObjectKind::NngSocket:
#endif
            return true;
        default:
            return false;
    }
}

Value DapServer::make_variable_json(const std::string& name, uint64_t val) {
    int var_ref = 0;
    if (is_compound_value(val)) {
        var_ref = alloc_compound_ref(val);
    }
    return json::object({
        {"name",               Value(name)},
        {"value",              Value(driver_->format_value(val))},
        {"variablesReference", Value(static_cast<int64_t>(var_ref))},
    });
}

Array DapServer::expand_compound(uint64_t val) {
    using namespace runtime::nanbox;
    using namespace runtime::memory::heap;
    using namespace runtime::types;

    Array children;
    auto& heap = driver_->heap();

    if (auto* cons = heap.try_get_as<ObjectKind::Cons, Cons>(
            static_cast<ObjectId>(ops::payload(val)))) {
        children.push_back(make_variable_json("car", cons->car));
        children.push_back(make_variable_json("cdr", cons->cdr));
        return children;
    }

    if (auto* vec = heap.try_get_as<ObjectKind::Vector, Vector>(
            static_cast<ObjectId>(ops::payload(val)))) {
        int limit = static_cast<int>((std::min)(vec->elements.size(), std::size_t{200}));
        for (int i = 0; i < limit; ++i) {
            children.push_back(make_variable_json(
                "[" + std::to_string(i) + "]", vec->elements[static_cast<std::size_t>(i)]));
        }
        if (vec->elements.size() > 200) {
            children.push_back(json::object({
                {"name",  "..."},
                {"value", Value("(" + std::to_string(vec->elements.size() - 200) + " more)")},
                {"variablesReference", 0},
            }));
        }
        return children;
    }

    if (auto* cl = heap.try_get_as<ObjectKind::Closure, Closure>(
            static_cast<ObjectId>(ops::payload(val)))) {
        if (cl->func) {
            children.push_back(json::object({
                {"name",  "function"},
                {"value", Value(cl->func->name.empty() ? "<lambda>" : cl->func->name)},
                {"variablesReference", 0},
            }));
        }
        for (std::size_t i = 0; i < cl->upvals.size(); ++i) {
            std::string uname;
            if (cl->func && i < cl->func->upval_names.size() && !cl->func->upval_names[i].empty()) {
                uname = cl->func->upval_names[i];
            } else {
                uname = "&" + std::to_string(i);
            }
            children.push_back(make_variable_json(uname, cl->upvals[i]));
        }
        return children;
    }

#ifdef ETA_HAS_NNG
    if (auto* ns = heap.try_get_as<ObjectKind::NngSocket, eta::nng::NngSocketPtr>(
            static_cast<ObjectId>(ops::payload(val)))) {
        children.push_back(json::object({
            {"name",  "protocol"},
            {"value", Value(std::string(eta::nng::protocol_name(ns->protocol)))},
            {"variablesReference", 0},
        }));
        children.push_back(json::object({
            {"name",  "listening"},
            {"value", Value(ns->listening ? std::string("true") : std::string("false"))},
            {"variablesReference", 0},
        }));
        children.push_back(json::object({
            {"name",  "dialed"},
            {"value", Value(ns->dialed ? std::string("true") : std::string("false"))},
            {"variablesReference", 0},
        }));
        children.push_back(json::object({
            {"name",  "closed"},
            {"value", Value(ns->closed ? std::string("true") : std::string("false"))},
            {"variablesReference", 0},
        }));
        children.push_back(json::object({
            {"name",  "pending-msgs"},
            {"value", Value(std::to_string(ns->pending_msgs.size()))},
            {"variablesReference", 0},
        }));
        return children;
    }
#endif

    return children;
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

std::string DapServer::current_module_from_frame(std::size_t frame_idx) {
    /**
     * Must be called with vm_mutex_ held.
     * Returns the Eta module name (e.g. "composition", "std.io") that owns the
     * function executing in the requested frame.
     *
     * Strategy: function names are "<mod_underscored>_init" or
     * "<mod_underscored>_lambda<N>..." where dots in the module name are
     * replaced by underscores.  We scan global_names_ to find the longest
     * module prefix whose underscore form is a prefix of the function name.
     */
    auto frames = driver_->vm().get_frames();
    if (frame_idx >= frames.size()) return "";
    const std::string& fn = frames[frame_idx].func_name;

    const auto& names = driver_->global_names();
    std::string best_mod;
    std::size_t best_len = 0;

    for (const auto& [slot, full_name] : names) {
        auto dot = full_name.rfind('.');
        if (dot == std::string::npos) continue;
        std::string mod = full_name.substr(0, dot);
        if (mod.size() <= best_len) continue; ///< can't beat current best

        /// Build the underscore form of the module name for matching
        std::string mod_ul = mod;
        for (char& c : mod_ul) if (c == '.') c = '_';

        /// Function name must start with mod_ul followed by '_' (or be exactly mod_ul)
        if (fn.size() >= mod_ul.size() &&
            fn.compare(0, mod_ul.size(), mod_ul) == 0 &&
            (fn.size() == mod_ul.size() || fn[mod_ul.size()] == '_')) {
            best_mod = mod;
            best_len = mod_ul.size();
        }
    }
    return best_mod;
}


/**
 */

void DapServer::handle_child_processes(const Value& id, const Value& /*args*/) {
    Array children;

#ifdef ETA_HAS_NNG
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (driver_ && driver_->process_manager()) {
        for (const auto& ci : driver_->process_manager()->list_children()) {
            children.push_back(json::object({
                {"pid",        Value(static_cast<int64_t>(ci.pid))},
                {"endpoint",   Value(ci.endpoint)},
                {"modulePath", Value(ci.module_path)},
                {"alive",      Value(ci.alive)},
            }));
        }
    }
#endif

    send_response(id, json::object({{"children", Value(std::move(children))}}));
}


} ///< namespace eta::dap

