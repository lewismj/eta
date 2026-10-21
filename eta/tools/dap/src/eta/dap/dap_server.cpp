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

[[nodiscard]] std::string format_actor_pid(const runtime::types::Pid& pid) {
    return std::to_string(pid.node_id)
        + "." + std::to_string(pid.actor_id)
        + "." + std::to_string(pid.incarnation);
}

[[nodiscard]] const char* actor_run_state_label(runtime::actor::ActorSystem::RunState state) noexcept {
    using RunState = runtime::actor::ActorSystem::RunState;
    switch (state) {
        case RunState::Runnable: return "runnable";
        case RunState::Running: return "running";
        case RunState::Waiting: return "waiting";
        case RunState::Exited: return "exited";
    }
    return "unknown";
}

[[nodiscard]] const char* actor_yield_reason_label(
    runtime::actor::ActorSystem::YieldReason reason) noexcept {
    using YieldReason = runtime::actor::ActorSystem::YieldReason;
    switch (reason) {
        case YieldReason::None: return "none";
        case YieldReason::BudgetExhausted: return "budget-exhausted";
        case YieldReason::BlockedOnReceive: return "blocked-on-receive";
        case YieldReason::Finished: return "finished";
        case YieldReason::Error: return "error";
    }
    return "unknown";
}

/**
 * Construction
 */

DapServer::DapServer() : in_(std::cin), out_(std::cout) {
}

DapServer::DapServer(std::istream& in, std::ostream& out) : in_(in), out_(out) {
}

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
        if (!msg_str) {
            break;
        }

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

    try {
        if (*cmd == "initialize")               handle_initialize(id, args);
        else if (*cmd == "launch")              handle_launch(id, args);
        else if (*cmd == "setBreakpoints")      handle_set_breakpoints(id, args);
        else if (*cmd == "setFunctionBreakpoints") handle_set_function_breakpoints(id, args);
        else if (*cmd == "breakpointLocations") handle_breakpoint_locations(id, args);
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
        else if (*cmd == "setVariable")         handle_set_variable(id, args);
        else if (*cmd == "restart")             handle_restart(id, args);
        else if (*cmd == "terminate")           handle_terminate(id, args);
        else if (*cmd == "terminateThreads")    handle_terminate_threads(id, args);
        else if (*cmd == "cancel")              handle_cancel(id, args);
        else if (*cmd == "completions")         handle_completions(id, args);
        else if (*cmd == "disconnect")          handle_disconnect(id, args);
        else if (*cmd == "eta/symbolDoc")       handle_symbol_doc(id, args);
        else if (*cmd == "disassemble")         handle_standard_disassemble(id, args);
        else if (*cmd == "eta/environment")     handle_environment(id, args);
        else if (*cmd == "eta/localMemory")     handle_local_memory(id, args);
        else if (*cmd == "eta/heapSnapshot")    handle_heap_inspector(id, args);
        else if (*cmd == "eta/inspectObject")   handle_inspect_object(id, args);
        else if (*cmd == "eta/disassemble")     handle_disassemble(id, args);
        else if (*cmd == "eta/childProcesses")  handle_child_processes(id, args);
        else {
            send_response(id, json::object({}));
        }
    } catch (const std::exception& e) {
        const std::string request = cmd.value_or("<unknown>");
        const std::string message =
            "[eta_dap] internal adapter error while handling '" + request + "': " + e.what();
        send_event("output", json::object({
            {"category", "stderr"},
            {"output", message + "\n"},
        }));
        send_error_response(id, 1999, message);
    } catch (...) {
        const std::string request = cmd.value_or("<unknown>");
        const std::string message =
            "[eta_dap] internal adapter error while handling '" + request + "': unknown exception";
        send_event("output", json::object({
            {"category", "stderr"},
            {"output", message + "\n"},
        }));
        send_error_response(id, 1999, message);
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
        {"supportsFunctionBreakpoints",         true},
        {"supportsConditionalBreakpoints",      true},
        {"supportsHitConditionalBreakpoints",   true},
        {"supportsLogPoints",                   true},
        {"supportsSetVariable",                 true},
        {"supportsRestartRequest",              true},
        {"supportsTerminateRequest",            true},
        {"supportsTerminateThreadsRequest",     true},
        {"supportsCancelRequest",               true},
        {"supportsSteppingGranularity",         true},
        {"supportsEvaluateForHovers",           true},
        {"supportsStepBack",                    false},
        {"supportsGotoTargetsRequest",          false},
        {"supportsBreakpointLocationsRequest",  true},
        {"supportsDisassembleRequest",          true},
        {"supportsCompletionsRequest",          true},
        /// Vectors and other indexed compounds expose `indexedVariables`/`namedVariables`
        /// in their `make_variable_json` output and honour `start`/`count` on `variables`.
        {"supportsVariablePaging",              true},
        {"supportsVariableType",                true},
        {"supportsEtaSymbolDocRequest",         true},
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

    launch_profile_ = args.get_string("profile").value_or("debug");
    if (launch_profile_ != "debug" && launch_profile_ != "release") {
        send_error_response(
            id, 1002, "launch: 'profile' must be 'debug' or 'release'");
        return;
    }

    script_path_ = fs::path(*program);
    {
        std::error_code ec;
        auto absolute = fs::absolute(script_path_, ec);
        if (!ec) script_path_ = absolute;
        script_path_ = script_path_.lexically_normal();
    }
    stop_on_entry_  = args.has("stopOnEntry") && args["stopOnEntry"].is_bool()
                      && args["stopOnEntry"].as_bool();
    launched_       = true;
    last_launch_args_ = args;

    send_response(id, json::object({}));

    /**
     * Per DAP spec: send "initialized" AFTER the launch response so VS Code
     * sends setBreakpoints and configurationDone AFTER script_path_ is set.
     */
    send_event("initialized", json::object({}));

    send_event("output", json::object({
        {"category", "console"},
        {"output", "[eta_dap] Launch: " + script_path_.string()
                       + " (profile=" + launch_profile_ + ")\n"},
    }));
    /// The VM is actually started on configurationDone.
}

/**
 */

void DapServer::handle_configuration_done(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({}));

    if (!launched_) {
        return; ///< no launch request yet (shouldn't happen)
    }
    start_vm_from_current_launch();
}

void DapServer::start_vm_from_current_launch() {

    auto script_dir = script_path_.parent_path();
    const bool script_is_etac = script_path_.extension() == ".etac";
    /// Build module search roots from the script's package context.
    auto resolver = interpreter::ModulePathResolver::from_args_or_env_at(
        "", script_dir.empty() ? fs::current_path() : script_dir);
    /**
     * Debugging source submissions prefers .eta for richer line mapping.
     * Debugging a compiled script keeps the default .etac-first behavior.
     */
    resolver.set_prefer_source(!script_is_etac);
    /// Log the module search path so the user can diagnose "module not found"
    {
        std::ostringstream msg;
        msg << "[eta_dap] Module search dirs:\n";
        if (resolver.empty()) {
            msg << session::dap_empty_module_path_warning();
        } else {
            for (const auto& d : resolver.dirs()) msg << "  " << d.string() << "\n";
        }
        send_event("output", json::object({{"category", "console"}, {"output", msg.str()}}));
    }


    /// Build the driver on the DAP thread (it will be moved-to below)
    auto drv = std::make_unique<session::Driver>(std::move(resolver));

    /// Register stop callback BEFORE loading prelude so the hook is in place
    install_stop_callback_for(drv->vm(), MAIN_THREAD_ID);

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
        register_main_thread_locked();
        install_pending_breakpoints();
    }

#ifdef ETA_HAS_NNG
    /**
     * Subscribe to spawn-thread events from the parent VM's ProcessManager so
     * we can attach a per-thread stop callback (and install breakpoints) on
     * each child VM as it comes up.  See class DapThread / register_actor_thread.
     */
    if (auto* pm = driver_->process_manager()) {
        pm->set_debug_listener([this](const eta::native::ActorThreadDebugEvent& ev) {
            using Kind = eta::native::ActorThreadDebugEvent::Kind;
            if (ev.kind == Kind::Started) {
                register_actor_thread(
                    static_cast<session::Driver*>(ev.driver),
                    static_cast<runtime::vm::VM*>(ev.vm),
                    ev.index,
                    ev.name);
            } else {
                unregister_actor_thread(static_cast<runtime::vm::VM*>(ev.vm));
            }
        });
    }
#endif
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

    std::optional<runtime::types::Pid> main_actor_pid;
    if (auto actor_system = driver_->vm().actor_system()) {
        main_actor_pid = actor_system->current_pid();
    }

    /// Launch the VM on a background thread
    vm_thread_ = std::thread([this, script_is_etac, main_actor_pid]() {
        auto* drv = driver_.get();
        auto actor_system = drv ? drv->vm().actor_system() : nullptr;

        if (actor_system) {
            bool bound_main_actor = false;
            if (main_actor_pid.has_value()) {
                bound_main_actor = actor_system->bind_current_thread_pid(*main_actor_pid);
            }
            if (!bound_main_actor) {
                auto registered = actor_system->register_current_thread_actor();
                if (!registered.has_value()) {
                    send_event("output", json::object({
                        {"category", "stderr"},
                        {"output",
                            "[eta_dap] warning: failed to bind actor identity on VM thread: "
                                + registered.error() + "\n"},
                    }));
                }
            }
        }

        /**
         * Refresh breakpoints immediately before script execution.
         * This keeps all pending requests applied without relying on
         * any prelude/module pre-load side effects.
         */
        {
            std::lock_guard<std::mutex> lk(vm_mutex_);
            install_pending_breakpoints();
        }
        /// Notify VS Code about any breakpoints that became verified pre-run
        notify_breakpoints_verified();

        if (script_is_etac) {
            send_event("output", json::object({
                {"category", "important"},
                {"output",
                    "[eta_dap] Debugging compiled .etac bytecode.\n"
                    "[eta_dap] Source breakpoints/line mapping depend on embedded debug spans.\n"
                    "[eta_dap] If line mapping is limited, use instruction stepping or rebuild without --no-debug.\n"},
            }));
        }

        /// Execute the script
        bool ok = script_is_etac ? drv->run_etac_file(script_path_) : drv->run_file(script_path_);

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

        if (actor_system) {
            actor_system->unbind_current_thread_pid();
        }
    });
}

/**
 * continue / next / stepIn / stepOut / pause
 */

void DapServer::handle_continue(const Value& id, const Value& args) {
    std::lock_guard<std::mutex> lk(vm_mutex_);
    DapThread* th = resolve_thread_arg_locked(args);
    /// Per DAP: allThreadsContinued indicates whether the resume affects all
    /// threads.  We resume only the targeted VM, so report false unless the
    /// IDE asked for the (only) main thread and there are no actor threads.
    const bool all_continued = th && th->is_main && threads_by_id_.size() == 1;
    send_response(id, json::object({{"allThreadsContinued", Value(all_continued)}}));
    if (th && th->vm) th->vm->resume();
}

void DapServer::handle_next(const Value& id, const Value& args) {
    send_response(id, json::object({}));
    std::lock_guard<std::mutex> lk(vm_mutex_);
    DapThread* th = resolve_thread_arg_locked(args);
    if (!th || !th->vm) return;
    auto granularity = args.get_string("granularity");
    if (granularity && *granularity == "instruction") {
        th->vm->step_over_instruction();
    } else {
        th->vm->step_over();
    }
}

void DapServer::handle_step_in(const Value& id, const Value& args) {
    send_response(id, json::object({}));
    std::lock_guard<std::mutex> lk(vm_mutex_);
    DapThread* th = resolve_thread_arg_locked(args);
    if (!th || !th->vm) return;
    auto granularity = args.get_string("granularity");
    if (granularity && *granularity == "instruction") {
        th->vm->step_in_instruction();
    } else {
        th->vm->step_in();
    }
}

void DapServer::handle_step_out(const Value& id, const Value& args) {
    send_response(id, json::object({}));
    std::lock_guard<std::mutex> lk(vm_mutex_);
    DapThread* th = resolve_thread_arg_locked(args);
    if (!th || !th->vm) return;
    /**
     * stepOut is depth-based in the VM debug core and already instruction-level
     * with respect to source lines. Accept the field for protocol compatibility.
     */
    (void) args.get_string("granularity");
    th->vm->step_out();
}

void DapServer::handle_pause(const Value& id, const Value& args) {
    send_response(id, json::object({}));
    std::lock_guard<std::mutex> lk(vm_mutex_);
    DapThread* th = resolve_thread_arg_locked(args);
    if (th && th->vm) th->vm->request_pause();
}

/**
 * restart
 */

void DapServer::handle_restart(const Value& id, const Value& args) {
    Value restart_launch_args = last_launch_args_;
    const Value* overlay = nullptr;
    if (args.is_object()) {
        if (args.has("arguments") && args["arguments"].is_object()) {
            overlay = &args["arguments"];
        } else if (args.has("program") || args.has("profile") || args.has("stopOnEntry")) {
            overlay = &args;
        }
    }
    if (overlay && overlay->is_object()) {
        if (!restart_launch_args.is_object()) {
            restart_launch_args = json::object({});
        }
        for (const auto& [key, value] : overlay->as_object()) {
            restart_launch_args.as_object()[key] = value;
        }
    }

    auto program = restart_launch_args.get_string("program");
    if (!program || program->empty()) {
        send_error_response(id, 2019, "restart requested before launch; no previous launch arguments are available");
        return;
    }

    send_response(id, json::object({}));

    /// Best-effort graceful stop of the prior VM run.
    {
        std::lock_guard<std::mutex> lk(vm_mutex_);
        if (driver_) {
            driver_->vm().resume();
        }
    }
    if (vm_thread_.joinable()) {
        vm_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lk(vm_mutex_);
        driver_.reset();
        clear_compound_refs();
    }

    script_path_ = fs::path(*program);
    {
        std::error_code ec;
        auto absolute = fs::absolute(script_path_, ec);
        if (!ec) script_path_ = absolute;
        script_path_ = script_path_.lexically_normal();
    }
    stop_on_entry_ = restart_launch_args.has("stopOnEntry")
                     && restart_launch_args["stopOnEntry"].is_bool()
                     && restart_launch_args["stopOnEntry"].as_bool();
    launch_profile_ = restart_launch_args.get_string("profile").value_or("debug");
    last_launch_args_ = restart_launch_args;
    launched_ = true;

    send_event("output", json::object({
        {"category", "console"},
        {"output", "[eta_dap] Restart: " + script_path_.string()
                       + " (profile=" + launch_profile_ + ")\n"},
    }));

    start_vm_from_current_launch();
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
 * terminateThreads
 */

void DapServer::handle_terminate_threads(const Value& id, const Value& args) {
    send_response(id, json::object({}));
#ifdef ETA_HAS_NNG
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (!driver_ || !driver_->process_manager()) return;
    auto* pm = driver_->process_manager();

    if (!args.has("threadIds") || !args["threadIds"].is_array()) {
        const auto threads = pm->list_threads();
        for (std::size_t i = 0; i < threads.size(); ++i) {
            (void) pm->terminate_thread_by_index(static_cast<int>(i));
        }
        return;
    }

    for (const auto& tid_v : args["threadIds"].as_array()) {
        if (!tid_v.is_int()) continue;
        const int tid = static_cast<int>(tid_v.as_int());
        if (tid == MAIN_THREAD_ID) continue;
        DapThread* th = find_thread_locked(tid);
        if (!th || th->pm_index < 0) continue;
        (void) pm->terminate_thread_by_index(th->pm_index);
    }
#else
    (void) args;
#endif
}

/**
 * cancel
 */

void DapServer::handle_cancel(const Value& id, const Value& args) {
    const auto request_id_opt = args.get_int("requestId");
    if (request_id_opt.has_value()) {
        const int64_t request_id = *request_id_opt;
        {
            std::lock_guard<std::mutex> lk(cancel_mutex_);
            cancelled_request_ids_.insert(request_id);
        }
        if (active_heap_snapshot_request_.load(std::memory_order_relaxed) == request_id) {
            cancel_active_heap_snapshot_.store(true, std::memory_order_relaxed);
        }
    }
    send_response(id, json::object({}));
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



void DapServer::handle_child_processes(const Value& id, const Value& /*args*/) {
    Array children;

    std::vector<native::ActorChildInfo> child_processes;
    std::vector<runtime::actor::ActorSystem::ProcessInfo> actor_processes;

    {
        std::lock_guard<std::mutex> lk(vm_mutex_);
        if (driver_) {
#ifdef ETA_HAS_NNG
            if (driver_->process_manager()) {
                child_processes = driver_->process_manager()->list_children();
            }
#endif
            auto actor_system = driver_->vm().actor_system();
            if (actor_system) {
                actor_processes = actor_system->list_processes();
            }
        }
    }

    for (const auto& ci : child_processes) {
        children.push_back(json::object({
            {"kind",       Value("process")},
            {"pid",        Value(static_cast<int64_t>(ci.pid))},
            {"endpoint",   Value(ci.endpoint)},
            {"modulePath", Value(ci.module_path)},
            {"alive",      Value(ci.alive)},
        }));
    }

    for (const auto& info : actor_processes) {
        children.push_back(json::object({
            {"kind",            Value("actor")},
            {"actorPid",        Value(format_actor_pid(info.pid))},
            {"alive",           Value(info.alive)},
            {"registeredName",  Value(info.registered_name)},
            {"mailboxLength",   Value(static_cast<int64_t>(info.mailbox_length))},
            {"state",           Value(actor_run_state_label(info.run_state))},
            {"lastYieldReason", Value(actor_yield_reason_label(info.last_yield_reason))},
        }));
    }

    send_response(id, json::object({{"children", Value(std::move(children))}}));
}


/**
 * Per-thread registry implementation
 */

DapThread* DapServer::find_thread_locked(int dap_thread_id) {
    auto it = threads_by_id_.find(dap_thread_id);
    if (it == threads_by_id_.end()) return nullptr;
    return &it->second;
}

DapThread* DapServer::resolve_thread_arg_locked(const Value& args) {
    int tid = MAIN_THREAD_ID;
    if (args.is_object() && args.has("threadId")) {
        if (auto v = args.get_int("threadId")) tid = static_cast<int>(*v);
    }
    return find_thread_locked(tid);
}

void DapServer::register_main_thread_locked() {
    if (!driver_) return;
    if (threads_by_id_.find(MAIN_THREAD_ID) != threads_by_id_.end()) return;
    DapThread th;
    th.dap_thread_id = MAIN_THREAD_ID;
    th.name          = "main";
    th.driver        = driver_.get();
    th.vm            = &driver_->vm();
    th.pm_index      = -1;
    th.is_main       = true;
    th.alive         = true;
    id_by_vm_[th.vm] = MAIN_THREAD_ID;
    threads_by_id_.emplace(MAIN_THREAD_ID, std::move(th));
    /// Per DAP spec, the IDE expects to discover the main thread via `threads`
    /// but emitting a `thread` event is harmless and helps some clients.
    /// (Sent without holding the lock by callers if needed; safe here since
    /// send_event uses output_mutex_, not vm_mutex_.)
}

int DapServer::register_actor_thread(session::Driver* drv,
                                     runtime::vm::VM* vm,
                                     int pm_index,
                                     std::string name) {
    if (!drv || !vm) return 0;
    int new_id = 0;
    {
        std::lock_guard<std::mutex> lk(vm_mutex_);
        new_id = next_dap_thread_id_++;
        DapThread th;
        th.dap_thread_id = new_id;
        th.name          = name.empty() ? ("actor-" + std::to_string(new_id)) : std::move(name);
        th.driver        = drv;
        th.vm            = vm;
        th.pm_index      = pm_index;
        th.is_main       = false;
        th.alive         = true;
        id_by_vm_[vm]    = new_id;
        threads_by_id_.emplace(new_id, std::move(th));

        /// Install per-thread breakpoints (resolved via this child's Driver).
        install_pending_breakpoints_on_locked(*drv);
    }

    /// Install the per-thread stop callback OUTSIDE the lock; the callback
    /// itself acquires vm_mutex_ when handling stop events.
    install_stop_callback_for(*vm, new_id);

    /// Notify the IDE that a new thread is now debuggable.
    send_event("thread", json::object({
        {"reason",   "started"},
        {"threadId", Value(static_cast<int64_t>(new_id))},
    }));
    return new_id;
}

void DapServer::unregister_actor_thread(runtime::vm::VM* vm) {
    int dead_id = 0;
    {
        std::lock_guard<std::mutex> lk(vm_mutex_);
        if (!vm) return;
        auto it = id_by_vm_.find(vm);
        if (it == id_by_vm_.end()) return;
        dead_id = it->second;
        id_by_vm_.erase(it);
        auto th_it = threads_by_id_.find(dead_id);
        if (th_it != threads_by_id_.end()) {
            /// Mark dead and erase: the VM is going away momentarily so any
            /// queued requests will hit a "thread not found" path.
            th_it->second.alive = false;
            threads_by_id_.erase(th_it);
        }
    }
    if (dead_id != 0 && dead_id != MAIN_THREAD_ID) {
        send_event("thread", json::object({
            {"reason",   "exited"},
            {"threadId", Value(static_cast<int64_t>(dead_id))},
        }));
    }
}

void DapServer::install_stop_callback_for(runtime::vm::VM& vm, int dap_thread_id) {
    vm.set_stop_callback([this, dap_thread_id](const runtime::vm::StopEvent& ev) {
        on_thread_stopped(dap_thread_id, ev);
    });
}

void DapServer::on_thread_stopped(int dap_thread_id, const runtime::vm::StopEvent& ev) {
    using runtime::vm::StopReason;

    std::string reason_str;
    switch (ev.reason) {
        case StopReason::Breakpoint: reason_str = "breakpoint"; break;
        case StopReason::Step:       reason_str = "step";       break;
        case StopReason::Pause:      reason_str = "pause";      break;
        case StopReason::Exception:  reason_str = "exception";  break;
        default:                     reason_str = "pause";      break;
    }

    bool should_emit_stopped = true;
    bool is_main = (dap_thread_id == MAIN_THREAD_ID);
    std::vector<std::string> deferred_output;

    {
        std::lock_guard<std::mutex> lk(vm_mutex_);

        /// Compound refs are owned by whichever thread paused last.  Clearing
        /// on every stop keeps the maps small and avoids stale ids leaking
        /// across stops on different threads.
        clear_compound_refs();

        DapThread* th = find_thread_locked(dap_thread_id);
        if (th && th->driver && ev.reason == StopReason::Breakpoint) {
            session::Driver& drv = *th->driver;
            const auto current_file = ev.span.file_id;
            const auto current_line = ev.span.start.line;

            bool matched_breakpoint = false;
            bool matched_stop_breakpoint = false;

            auto consume_match = [&](int bp_id,
                                     std::string& condition,
                                     std::string& hit_condition,
                                     std::string& log_message,
                                     int& hit_count) {
                matched_breakpoint = true;
                ++hit_count;

                bool hit_ok = true;
                if (!hit_condition.empty()) {
                    std::string hit_err;
                    if (!matches_hit_condition(hit_condition, hit_count, hit_ok, hit_err)) {
                        hit_ok = true;
                        deferred_output.push_back(
                            "[eta_dap] breakpoint " + std::to_string(bp_id)
                            + ": invalid hitCondition '" + hit_condition
                            + "' (" + hit_err + "); treating as unconditional.");
                    }
                }
                if (!hit_ok) return;

                bool cond_ok = true;
                if (!condition.empty()) {
                    std::string cond_err;
                    if (!eval_breakpoint_condition(drv, condition, cond_ok, cond_err)) {
                        cond_ok = true;
                        deferred_output.push_back(
                            "[eta_dap] breakpoint " + std::to_string(bp_id)
                            + ": invalid condition '" + condition
                            + "' (" + cond_err + "); treating as unconditional.");
                    }
                }
                if (!cond_ok) return;

                if (!log_message.empty()) {
                    deferred_output.push_back(render_logpoint_message(drv, log_message) + "\n");
                    return;
                }

                matched_stop_breakpoint = true;
            };

            for (auto& [path, bps] : pending_bps_) {
                const uint32_t file_id = drv.file_id_for_path(path);
                if (file_id == 0 || file_id != current_file) continue;
                for (auto& bp : bps) {
                    if (static_cast<uint32_t>(bp.line) != current_line) continue;
                    consume_match(bp.id, bp.condition, bp.hit_condition, bp.log_message, bp.hit_count);
                }
            }

            /// Function breakpoints are resolved against the *parent* driver
            /// (the one that compiled the script and knows global names).
            /// Only the main VM hits these meaningfully; for child VMs they
            /// are inert until we wire up per-driver function-bp resolution.
            if (is_main) {
                for (auto& bp : pending_function_bps_) {
                    uint32_t file_id = 0;
                    uint32_t line = 0;
                    if (!resolve_function_breakpoint(bp.name, file_id, line, nullptr)) continue;
                    if (file_id != current_file || line != current_line) continue;

                    std::string no_log;
                    consume_match(bp.id, bp.condition, bp.hit_condition, no_log, bp.hit_count);
                }
            }

            if (matched_breakpoint && !matched_stop_breakpoint) {
                should_emit_stopped = false;
                th->vm->resume();
            }
        }
    }

    for (const auto& text : deferred_output) {
        send_event("output", json::object({
            {"category", "console"},
            {"output", text},
        }));
    }

    if (!should_emit_stopped) {
        return;
    }

    send_event("stopped", json::object({
        {"reason",            reason_str},
        {"threadId",          Value(static_cast<int64_t>(dap_thread_id))},
        {"allThreadsStopped", Value(is_main)},
    }));
}
} ///< namespace eta::dap

