#include "dap_server.h"
#include "dap_io.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>

/// Eta interpreter
#include "eta/session/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/runtime/port.h"
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/vm/disassembler.h"
#include "eta/runtime/vm/sandbox.h"
#include "eta/runtime/value_formatter.h"
#include "eta/runtime/memory/mark_sweep_gc.h"
#include "eta/runtime/memory/cons_pool.h"
#include "eta/runtime/types/types.h"
#include "eta/diagnostic/diagnostic.h"

#ifdef ETA_HAS_NNG
#include "eta/nng/nng_socket_ptr.h"
#include "eta/nng/nng_primitives.h"
#include "eta/nng/process_mgr.h"
#endif

namespace eta::dap {

namespace fs = std::filesystem;
using namespace eta::json;

/**
 * Local helpers
 */

static const fs::path& dap_debug_log_path() {
    static const fs::path path = []() {
        if (const char* env = std::getenv("ETA_DAP_DEBUG_LOG_PATH")) {
            if (*env != '\0') return fs::path(env);
        }
#ifdef _WIN32
        return fs::path(R"(C:\tmp\dap_server_debug.txt)");
#else
        std::error_code ec;
        fs::path tmp = fs::temp_directory_path(ec);
        if (ec) tmp = fs::path("/tmp");
        return tmp / "dap_server_debug.txt";
#endif
    }();
    return path;
}

static void dap_debug_log(const std::string& text) {
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lk(log_mutex);

    const fs::path& path = dap_debug_log_path();
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }

    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) return;

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    out << now_ms << " [tid " << std::this_thread::get_id() << "] " << text << "\n";
    out.flush();
}

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

DapServer::DapServer() : in_(std::cin), out_(std::cout) {
    dap_debug_log("DapServer::DapServer() path=" + dap_debug_log_path().string());
}

DapServer::DapServer(std::istream& in, std::ostream& out) : in_(in), out_(out) {
    dap_debug_log("DapServer::DapServer(stream,stream) path=" + dap_debug_log_path().string());
}

/**
 * Destruction
 */

DapServer::~DapServer() {
    dap_debug_log("DapServer::~DapServer begin");
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
    dap_debug_log("DapServer::~DapServer end");
}

/**
 * Transport
 */

void DapServer::send(const Value& msg) {
    std::lock_guard<std::mutex> lk(output_mutex_);
    write_message(out_, json::to_string(msg));
}

void DapServer::send_response(const Value& id, const Value& body) {
    const std::string req = id.is_int() ? std::to_string(id.as_int()) : "?";
    dap_debug_log("send_response cmd=" + current_command_ + " request_seq=" + req);
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
    const std::string req = id.is_int() ? std::to_string(id.as_int()) : "?";
    dap_debug_log(
        "send_error_response cmd=" + current_command_
        + " request_seq=" + req
        + " code=" + std::to_string(code)
        + " msg=" + msg
    );
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
    dap_debug_log("send_event name=" + event_name);
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
    dap_debug_log("run() begin");
    while (running_) {
        auto msg_str = read_message(in_);
        if (!msg_str) {
            dap_debug_log("run() read_message: EOF");
            break;
        }
        dap_debug_log("run() read_message bytes=" + std::to_string(msg_str->size()));

        try {
            auto msg = json::parse(*msg_str);
            dispatch(msg);
        } catch (const std::exception& e) {
            dap_debug_log(std::string("run() parse/dispatch exception: ") + e.what());
            std::cerr << "[eta_dap] parse error: " << e.what() << "\n";
        }
    }

    /// Join the VM thread if still running
    if (vm_thread_.joinable()) {
        vm_thread_.join();
    }
    dap_debug_log("run() end");
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
    const std::string req = id.is_int() ? std::to_string(id.as_int()) : "?";
    dap_debug_log("dispatch begin cmd=" + *cmd + " request_seq=" + req);

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
        else if (*cmd == "disassemble")         handle_standard_disassemble(id, args);
        else if (*cmd == "eta/heapSnapshot")    handle_heap_inspector(id, args);
        else if (*cmd == "eta/inspectObject")   handle_inspect_object(id, args);
        else if (*cmd == "eta/disassemble")     handle_disassemble(id, args);
        else if (*cmd == "eta/childProcesses")  handle_child_processes(id, args);
        else {
            send_response(id, json::object({}));
        }
    } catch (const std::exception& e) {
        dap_debug_log(std::string("dispatch exception cmd=") + *cmd + " err=" + e.what());
        throw;
    }
    dap_debug_log("dispatch end cmd=" + *cmd + " request_seq=" + req);
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
        {"supportsVariableType",                false},
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
    last_launch_args_ = args;

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
    std::vector<PendingBp> next_for_path;

    if (args.has("breakpoints") && args["breakpoints"].is_array()) {
        for (const auto& bp : args["breakpoints"].as_array()) {
            auto line_opt = bp.get_int("line");
            if (!line_opt) continue;
            int line  = static_cast<int>(*line_opt);
            int bp_id = next_bp_id_++;
            PendingBp pb;
            pb.line = line;
            pb.id = bp_id;
            if (auto cond = bp.get_string("condition")) {
                pb.condition = *cond;
            }
            if (auto hit_cond = bp.get_string("hitCondition")) {
                pb.hit_condition = *hit_cond;
            }
            if (auto log_message = bp.get_string("logMessage")) {
                pb.log_message = *log_message;
            }
            next_for_path.push_back(std::move(pb));

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
    bool has_driver = false;
    {
        std::lock_guard<std::mutex> lk(vm_mutex_);
        pending_bps_[canon_path] = std::move(next_for_path);
        has_driver = (driver_ != nullptr);
        if (has_driver) {
            install_pending_breakpoints();
        }
    }
    if (has_driver) {
        notify_breakpoints_verified();
        /// Mark returned breakpoints as verified in the response too
        for (auto& bp_val : result_bps) {
            if (bp_val.is_object()) bp_val.as_object()["verified"] = Value(true);
        }
    }

    send_response(id, json::object({{"breakpoints", Value(std::move(result_bps))}}));
}

static std::string trim_copy(std::string s) {
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

static bool try_parse_i64(std::string_view text, int64_t& out) {
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    auto parsed = std::from_chars(begin, end, out, 10);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

static bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ac = static_cast<unsigned char>(a[i]);
        const auto bc = static_cast<unsigned char>(b[i]);
        if (std::tolower(ac) != std::tolower(bc)) return false;
    }
    return true;
}

/**
 * setFunctionBreakpoints
 */

void DapServer::handle_set_function_breakpoints(const Value& id, const Value& args) {
    std::vector<PendingFunctionBp> next_function_bps;

    Array result_bps;
    if (args.has("breakpoints") && args["breakpoints"].is_array()) {
        for (const auto& bp : args["breakpoints"].as_array()) {
            auto name_opt = bp.get_string("name");
            if (!name_opt || name_opt->empty()) continue;

            const int bp_id = next_bp_id_++;
            PendingFunctionBp pfb;
            pfb.name = *name_opt;
            pfb.id = bp_id;
            if (auto cond = bp.get_string("condition")) {
                pfb.condition = *cond;
            }
            if (auto hit_cond = bp.get_string("hitCondition")) {
                pfb.hit_condition = *hit_cond;
            }
            next_function_bps.push_back(std::move(pfb));

            bool verified = false;
            uint32_t file_id = 0;
            uint32_t line = 0;
            std::string message;
            std::string source_path;
            std::string source_name;
            {
                std::lock_guard<std::mutex> lk(vm_mutex_);
                if (driver_) {
                    verified = resolve_function_breakpoint(*name_opt, file_id, line, &message);
                    if (verified) {
                        if (const auto* p = driver_->path_for_file_id(file_id)) {
                            source_path = p->string();
                            source_name = p->filename().string();
                        }
                    }
                }
            }

            Object bp_obj{
                {"id", Value(static_cast<int64_t>(bp_id))},
                {"verified", Value(verified)},
            };
            if (verified) {
                bp_obj["line"] = Value(static_cast<int64_t>(line));
                if (!source_path.empty()) {
                    bp_obj["source"] = json::object({
                        {"name", source_name},
                        {"path", source_path},
                    });
                }
            } else if (!message.empty()) {
                bp_obj["message"] = Value(message);
            }
            result_bps.push_back(Value(std::move(bp_obj)));
        }
    }

    bool has_driver = false;
    {
        std::lock_guard<std::mutex> lk(vm_mutex_);
        pending_function_bps_ = std::move(next_function_bps);
        has_driver = (driver_ != nullptr);
        if (has_driver) {
            install_pending_breakpoints();
        }
    }
    if (has_driver) {
        notify_breakpoints_verified();
    }

    send_response(id, json::object({{"breakpoints", Value(std::move(result_bps))}}));
}

/**
 * breakpointLocations
 */

void DapServer::handle_breakpoint_locations(const Value& id, const Value& args) {
    auto source_obj = args["source"];
    auto path_val   = source_obj.get_string("path");
    if (!path_val) {
        send_response(id, json::object({{"breakpoints", json::array({})}}));
        return;
    }

    auto start_line_opt = args.get_int("line");
    if (!start_line_opt || *start_line_opt <= 0) {
        send_response(id, json::object({{"breakpoints", json::array({})}}));
        return;
    }

    int start_line = static_cast<int>(*start_line_opt);
    int end_line = start_line;
    if (auto end_line_opt = args.get_int("endLine"); end_line_opt && *end_line_opt > 0) {
        end_line = static_cast<int>(*end_line_opt);
    }
    if (end_line < start_line) std::swap(start_line, end_line);

    std::set<uint32_t> valid_lines;
    {
        std::lock_guard<std::mutex> lk(vm_mutex_);
        if (driver_) {
            auto file_id = driver_->file_id_for_path(*path_val);
            valid_lines = driver_->valid_lines_for(file_id);
        }
    }

    Array matches;
    for (uint32_t line : valid_lines) {
        if (line < static_cast<uint32_t>(start_line) || line > static_cast<uint32_t>(end_line))
            continue;
        matches.push_back(json::object({
            {"line", Value(static_cast<int64_t>(line))},
        }));
    }

    send_response(id, json::object({{"breakpoints", Value(std::move(matches))}}));
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
    dap_debug_log("handle_configuration_done begin launched=" + std::string(launched_ ? "true" : "false"));
    send_response(id, json::object({}));

    if (!launched_) {
        dap_debug_log("handle_configuration_done early-return (launch not seen)");
        return; ///< no launch request yet (shouldn't happen)
    }
    dap_debug_log("handle_configuration_done starting VM bootstrap");
    start_vm_from_current_launch();
    dap_debug_log("handle_configuration_done end");
}

void DapServer::start_vm_from_current_launch() {
    dap_debug_log("start_vm_from_current_launch begin script=" + script_path_.string());

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
            msg << "  (none  -  prelude will not load; set eta.lsp.modulePath in VS Code settings)\n";
        } else {
            for (const auto& d : resolver.dirs()) msg << "  " << d.string() << "\n";
        }
        send_event("output", json::object({{"category", "console"}, {"output", msg.str()}}));
    }


    /// Build the driver on the DAP thread (it will be moved-to below)
    auto drv = std::make_unique<session::Driver>(std::move(resolver));
    dap_debug_log("start_vm_from_current_launch created Driver");

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
        dap_debug_log("start_vm_from_current_launch acquired vm_mutex to publish driver");
        driver_ = std::move(drv);
        register_main_thread_locked();
        install_pending_breakpoints();
    }
    dap_debug_log("start_vm_from_current_launch published driver and breakpoints");

#ifdef ETA_HAS_NNG
    /**
     * Subscribe to spawn-thread events from the parent VM's ProcessManager so
     * we can attach a per-thread stop callback (and install breakpoints) on
     * each child VM as it comes up.  See class DapThread / register_actor_thread.
     */
    if (auto* pm = driver_->process_manager()) {
        pm->set_debug_listener([this](const eta::nng::ProcessManager::ThreadDebugEvent& ev) {
            using Kind = eta::nng::ProcessManager::ThreadDebugEvent::Kind;
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
        dap_debug_log("start_vm_from_current_launch requesting stop-on-entry pause");
        driver_->vm().request_pause();
    }

    /// Launch the VM on a background thread
    vm_thread_ = std::thread([this]() {
        dap_debug_log("vm_thread begin");
        auto* drv = driver_.get();

        /// Load prelude
        dap_debug_log("vm_thread loading prelude");
        auto pr = drv->load_prelude();
        if (!pr.found) {
            send_event("output", json::object({
                {"category", "important"},
                {"output",
                    "[eta_dap] Warning: prelude.eta not found  -  std.core, std.io, std.prelude etc. unavailable.\n"
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
        dap_debug_log("vm_thread run_file begin script=" + script_path_.string());
        bool ok = drv->run_file(script_path_);
        dap_debug_log("vm_thread run_file end ok=" + std::string(ok ? "true" : "false"));

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
        dap_debug_log("vm_thread end");
    });
    dap_debug_log("start_vm_from_current_launch end (vm thread launched)");
}

/**
 * threads
 */

void DapServer::handle_threads(const Value& id, const Value& /*args*/) {
    Array threads;
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (threads_by_id_.empty()) {
        /// Even without a launch we always advertise a main thread so VS Code
        /// can render an empty Call Stack pane without a "No threads" warning.
        threads.push_back(json::object({{"id", 1}, {"name", "main"}}));
    } else {
        /// Emit threads in id order so the IDE displays them stably.
        std::vector<int> ids;
        ids.reserve(threads_by_id_.size());
        for (const auto& [id_k, _] : threads_by_id_) ids.push_back(id_k);
        std::sort(ids.begin(), ids.end());
        for (int tid : ids) {
            const auto& th = threads_by_id_.at(tid);
            threads.push_back(json::object({
                {"id",   Value(static_cast<int64_t>(th.dap_thread_id))},
                {"name", Value(th.name)},
            }));
        }
    }
    send_response(id, json::object({{"threads", Value(std::move(threads))}}));
}

/**
 * stackTrace
 */

void DapServer::handle_stack_trace(const Value& id, const Value& args) {
    std::lock_guard<std::mutex> lk(vm_mutex_);
    DapThread* th = resolve_thread_arg_locked(args);
    if (!th || !th->vm || !th->vm->is_paused()) {
        send_response(id, json::object({{"stackFrames", json::array({})}, {"totalFrames", 0}}));
        return;
    }

    session::Driver& drv = *th->driver;
    auto frames = th->vm->get_frames();
    Array frames_arr;
    int frame_idx = 0;
    /// Encode threadId into frameId so subsequent scopes / evaluate requests
    /// can route back to the correct VM (DAP only carries frameId, not
    /// threadId, in those follow-up calls).
    const int tid_high = (th->dap_thread_id & 0xFFF) << 16;
    for (const auto& fi : frames) {
        const auto& sp = fi.span;
        Value source_val = source_json_for(drv, sp.file_id);

        const int frame_id = tid_high | (frame_idx & 0xFFFF);
        frames_arr.push_back(json::object({
            {"id",     Value(static_cast<int64_t>(frame_id))},
            {"name",   fi.func_name.empty() ? Value("<anonymous>") : Value(fi.func_name)},
            {"source", source_val},
            {"line",   Value(static_cast<int64_t>(sp.start.line))},
            {"column", Value(static_cast<int64_t>(sp.start.column))},
        }));
        ++frame_idx;
    }

    send_response(id, json::object({
        {"stackFrames", Value(std::move(frames_arr))},
        {"totalFrames", Value(static_cast<int64_t>(frame_idx))},
    }));
}

/**
 * scopes
 */

void DapServer::handle_scopes(const Value& id, const Value& args) {
    auto frame_id_opt = args.get_int("frameId");
    int frame_id = frame_id_opt ? static_cast<int>(*frame_id_opt) : 0;

    /// Frame ids carry the thread id in their high bits (see handle_stack_trace).
    int thread_id = (frame_id >> 16) & 0xFFF;
    if (thread_id == 0) thread_id = MAIN_THREAD_ID;
    int frame_idx = frame_id & 0xFFFF;

    send_response(id, json::object({
        {"scopes", json::array({
            json::object({
                {"name",               "Module"},
                {"variablesReference", Value(static_cast<int64_t>(encode_var_ref(thread_id, frame_idx, 3)))},
                {"expensive",          false},
                {"presentationHint",   "locals"},
            }),
            json::object({
                {"name",               "Locals"},
                {"variablesReference", Value(static_cast<int64_t>(encode_var_ref(thread_id, frame_idx, 0)))},
                {"expensive",          false},
            }),
            json::object({
                {"name",               "Upvalues"},
                {"variablesReference", Value(static_cast<int64_t>(encode_var_ref(thread_id, frame_idx, 1)))},
                {"expensive",          false},
            }),
            json::object({
                {"name",               "Globals"},
                {"variablesReference", Value(static_cast<int64_t>(encode_var_ref(thread_id, frame_idx, 2)))},
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

    auto ref_opt = args.get_int("variablesReference");
    if (!ref_opt) {
        send_response(id, json::object({{"variables", json::array({})}}));
        return;
    }
    int ref = static_cast<int>(*ref_opt);

    /// Compound variable expansion (cons/vector/closure)  -  owning thread is
    /// recorded in the CompoundRef so we look up the right Driver/heap.
    if (ref >= COMPOUND_REF_BASE) {
        auto cit = compound_refs_.find(ref);
        if (cit == compound_refs_.end()) {
            send_response(id, json::object({{"variables", json::array({})}}));
            return;
        }
        DapThread* th = find_thread_locked(cit->second.dap_thread_id);
        if (!th || !th->driver) {
            send_response(id, json::object({{"variables", json::array({})}}));
            return;
        }
        const auto start_opt = args.get_int("start");
        const auto count_opt = args.get_int("count");
        const int  start = start_opt ? static_cast<int>(*start_opt) : 0;
        const int  count = count_opt ? static_cast<int>(*count_opt) : 0;
        auto children = expand_compound(*th->driver, th->dap_thread_id, cit->second.value, start, count);
        send_response(id, json::object({{"variables", Value(std::move(children))}}));
        return;
    }

    /// Frame scope variables (locals / upvalues / globals).
    int thread_id = decode_var_ref_thread(ref);
    int frame_idx = decode_var_ref_frame(ref);
    int scope     = decode_var_ref_scope(ref);

    DapThread* th = find_thread_locked(thread_id);
    if (!th || !th->vm || !th->driver || !th->vm->is_paused()) {
        send_response(id, json::object({{"variables", json::array({})}}));
        return;
    }
    session::Driver& drv = *th->driver;
    runtime::vm::VM& vm = *th->vm;

    if (scope == 3) {
        /**
         * Module scope: globals that belong to the currently-executing module
         * so they are immediately visible without scrolling through all prelude/std.* entries.
         */
        std::string cur_mod = current_module_from_frame(drv, static_cast<std::size_t>(frame_idx));
        const auto& globals = vm.globals();
        const auto& names   = drv.global_names();
        Array vars;
        for (const auto& [slot, full_name] : names) {
            if (slot >= globals.size()) continue;
            auto v = globals[slot];
            if (v == runtime::nanbox::Nil) continue;
            auto dot = full_name.rfind('.');
            if (dot == std::string::npos) continue;
            if (full_name.substr(0, dot) != cur_mod) continue;
            vars.push_back(make_variable_json(drv, thread_id, full_name.substr(dot + 1), v));
        }
        send_response(id, json::object({{"variables", Value(std::move(vars))}}));
        return;
    }

    if (scope == 2) {
        /// Globals scope
        const auto& globals = vm.globals();
        const auto& names   = drv.global_names();
        Array vars;
        for (std::size_t slot = 0; slot < globals.size(); ++slot) {
            auto v = globals[slot];
            if (v == runtime::nanbox::Nil) continue;

            auto it = names.find(static_cast<uint32_t>(slot));
            std::string name = (it != names.end()) ? it->second
                                                   : "global[" + std::to_string(slot) + "]";
            vars.push_back(make_variable_json(drv, thread_id, name, v));
        }
        send_response(id, json::object({{"variables", Value(std::move(vars))}}));
        return;
    }

    auto entries = (scope == 0)
                   ? vm.get_locals(static_cast<std::size_t>(frame_idx))
                   : vm.get_upvalues(static_cast<std::size_t>(frame_idx));

    Array vars;
    for (const auto& e : entries) {
        if (!e.name.empty() && e.name[0] == '%' && e.value == runtime::nanbox::Nil)
            continue;
        vars.push_back(make_variable_json(drv, thread_id, e.name, e.value));
    }

    send_response(id, json::object({{"variables", Value(std::move(vars))}}));
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
 * setVariable
 */

void DapServer::handle_set_variable(const Value& id, const Value& args) {
    const auto ref_opt = args.get_int("variablesReference");
    const auto name_opt = args.get_string("name");
    const auto value_opt = args.get_string("value");

    if (!ref_opt || !name_opt || !value_opt) {
        send_error_response(id, 2010, "setVariable requires variablesReference, name, and value");
        return;
    }

    const int ref = static_cast<int>(*ref_opt);
    if (ref <= 0 || ref >= COMPOUND_REF_BASE) {
        send_error_response(id, 2011, "setVariable only supports locals/module/globals scopes");
        return;
    }

    std::lock_guard<std::mutex> lk(vm_mutex_);
    int thread_id = decode_var_ref_thread(ref);
    DapThread* th = find_thread_locked(thread_id);
    if (!th || !th->driver || !th->vm) {
        send_error_response(id, 2001, "VM not running");
        return;
    }
    if (!th->vm->is_paused()) {
        send_error_response(id, 2002, "VM must be paused to set variable values");
        return;
    }
    session::Driver& drv = *th->driver;
    runtime::vm::VM& vm = *th->vm;

    uint64_t new_value = runtime::nanbox::Nil;
    std::string parse_error;
    if (!parse_set_variable_value(drv, *value_opt, new_value, parse_error)) {
        send_error_response(id, 2012, "setVariable: " + parse_error);
        return;
    }

    const int frame_idx = decode_var_ref_frame(ref);
    const int scope = decode_var_ref_scope(ref);

    const std::string requested_name = *name_opt;
    std::optional<uint32_t> global_slot;

    auto resolve_global_slot = [&](bool module_scope) -> std::optional<uint32_t> {
        const auto& names = drv.global_names();
        const auto& globals = vm.globals();

        auto exact_full = [&](const std::string& full_name) -> std::optional<uint32_t> {
            for (const auto& [slot, name] : names) {
                if (slot < globals.size() && name == full_name) return slot;
            }
            return std::nullopt;
        };

        if (module_scope) {
            const std::string cur_mod = current_module_from_frame(drv, static_cast<std::size_t>(frame_idx));
            if (cur_mod.empty()) return std::nullopt;
            if (requested_name.find('.') != std::string::npos) {
                return exact_full(requested_name);
            }
            return exact_full(cur_mod + "." + requested_name);
        }

        if (requested_name.find('.') != std::string::npos) {
            return exact_full(requested_name);
        }

        std::optional<uint32_t> found;
        for (const auto& [slot, full_name] : names) {
            if (slot >= globals.size()) continue;
            const auto dot = full_name.rfind('.');
            const std::string short_name = (dot == std::string::npos)
                ? full_name
                : full_name.substr(dot + 1);
            if (short_name != requested_name) continue;
            if (found.has_value()) return std::nullopt; ///< ambiguous short name
            found = slot;
        }
        return found;
    };

    switch (scope) {
        case 0: {
            const auto locals = vm.get_locals(static_cast<std::size_t>(frame_idx));
            std::optional<std::size_t> slot;
            for (std::size_t i = 0; i < locals.size(); ++i) {
                if (locals[i].name == requested_name) {
                    slot = i;
                    break;
                }
            }
            if (!slot.has_value()) {
                send_error_response(id, 2013, "setVariable: local not found: " + requested_name);
                return;
            }
            if (!vm.set_local(static_cast<std::size_t>(frame_idx), *slot, new_value)) {
                send_error_response(id, 2014, "setVariable: failed to update local slot");
                return;
            }
            break;
        }
        case 1:
            send_error_response(id, 2015, "setVariable: upvalues are read-only");
            return;
        case 2:
            global_slot = resolve_global_slot(false);
            break;
        case 3:
            global_slot = resolve_global_slot(true);
            break;
        default:
            send_error_response(id, 2016, "setVariable: unsupported scope");
            return;
    }

    if ((scope == 2 || scope == 3)) {
        if (!global_slot.has_value()) {
            send_error_response(id, 2017, "setVariable: global not found (or ambiguous): " + requested_name);
            return;
        }
        auto& globals = vm.globals();
        if (*global_slot >= globals.size()) {
            send_error_response(id, 2018, "setVariable: resolved global slot is out of range");
            return;
        }
        globals[*global_slot] = new_value;
    }

    const int vr = is_compound_value(new_value) ? alloc_compound_ref(new_value, thread_id) : 0;
    send_response(id, json::object({
        {"value",              Value(drv.format_value(new_value))},
        {"variablesReference", Value(static_cast<int64_t>(vr))},
    }));
}

/**
 * restart
 */

void DapServer::handle_restart(const Value& id, const Value& /*args*/) {
    auto program = last_launch_args_.get_string("program");
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
    stop_on_entry_ = last_launch_args_.has("stopOnEntry")
                     && last_launch_args_["stopOnEntry"].is_bool()
                     && last_launch_args_["stopOnEntry"].as_bool();
    launched_ = true;

    send_event("output", json::object({
        {"category", "console"},
        {"output", "[eta_dap] Restart: " + script_path_.string() + "\n"},
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
            pm->terminate_thread_by_index(static_cast<int>(i));
        }
        return;
    }

    for (const auto& tid_v : args["threadIds"].as_array()) {
        if (!tid_v.is_int()) continue;
        const int tid = static_cast<int>(tid_v.as_int());
        if (tid == MAIN_THREAD_ID) continue;
        DapThread* th = find_thread_locked(tid);
        if (!th || th->pm_index < 0) continue;
        pm->terminate_thread_by_index(th->pm_index);
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

bool DapServer::eval_breakpoint_condition(session::Driver& drv,
                                          const std::string& condition,
                                          bool& out_truthy,
                                          std::string& out_error) {
    out_truthy = true;
    out_error.clear();

    const std::string expr = trim_copy(condition);
    if (expr.empty()) return true;

    if (expr == "#t" || iequals(expr, "true")) {
        out_truthy = true;
        return true;
    }
    if (expr == "#f" || iequals(expr, "false")) {
        out_truthy = false;
        return true;
    }

    int64_t n = 0;
    if (try_parse_i64(expr, n)) {
        out_truthy = (n != 0);
        return true;
    }

    if (!is_identifier_expr(expr)) {
        std::string formatted;
        uint64_t    val = runtime::nanbox::Nil;
        std::string sandbox_err;
        bool        violation = false;
        if (eval_in_paused_frame(drv, 0, expr, formatted, &val, &sandbox_err, &violation)) {
            out_truthy = (val != runtime::nanbox::False);
            return true;
        }
        out_error = violation
            ? "sandbox-violation: " + sandbox_err
            : sandbox_err;
        return false;
    }

    uint64_t val = runtime::nanbox::Nil;
    if (!try_lookup_paused_name(drv, expr, val)) {
        out_truthy = false;
        return true;
    }

    out_truthy = (val != runtime::nanbox::False);
    return true;
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

    auto& vm = drv.vm();
    vm.set_sandbox_mode(true);
    auto sandbox_result = sandbox.eval(expr);
    vm.set_sandbox_mode(false);

    if (!sandbox_result.ok()) {
        if (out_error)     *out_error     = sandbox_result.error;
        if (out_violation) *out_violation = sandbox_result.violation;
        return false;
    }

    if (out_val) *out_val = sandbox_result.value;
    out_str = drv.format_value(sandbox_result.value);
    return true;
}

bool DapServer::parse_set_variable_value(session::Driver& drv,
                                         const std::string& value_text,
                                         uint64_t& out_value,
                                         std::string& out_error) {
    out_error.clear();
    const std::string text = trim_copy(value_text);
    if (text.empty()) {
        out_error = "value must not be empty";
        return false;
    }

    if (text == "#t" || iequals(text, "true")) {
        out_value = runtime::nanbox::True;
        return true;
    }
    if (text == "#f" || iequals(text, "false")) {
        out_value = runtime::nanbox::False;
        return true;
    }
    if (text == "nil" || text == "()") {
        out_value = runtime::nanbox::Nil;
        return true;
    }

    int64_t as_int = 0;
    if (try_parse_i64(text, as_int)) {
        auto enc = runtime::nanbox::ops::encode<int64_t>(as_int);
        if (!enc) {
            out_error = "integer out of range for Eta fixnum";
            return false;
        }
        out_value = *enc;
        return true;
    }

    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        const std::string raw = text.substr(1, text.size() - 2);
        auto sid = drv.intern_table().intern(raw);
        if (!sid) {
            out_error = "failed to intern string value";
            return false;
        }
        out_value = runtime::nanbox::ops::box(runtime::nanbox::Tag::String, *sid);
        return true;
    }

    if (text.find('.') != std::string::npos ||
        text.find('e') != std::string::npos ||
        text.find('E') != std::string::npos) {
        char* end = nullptr;
        const double as_double = std::strtod(text.c_str(), &end);
        if (end != text.c_str() && end != nullptr && *end == '\0') {
            auto enc = runtime::nanbox::ops::encode<double>(as_double);
            if (!enc) {
                out_error = "invalid floating-point value";
                return false;
            }
            out_value = *enc;
            return true;
        }
    }

    if (is_identifier_expr(text) && try_lookup_paused_name(drv, text, out_value)) {
        return true;
    }

    {
        std::string formatted;
        std::string sandbox_err;
        bool        violation = false;
        if (eval_in_paused_frame(drv, 0, text, formatted, &out_value, &sandbox_err, &violation)) {
            return true;
        }
        out_error = violation
            ? "sandbox-violation: " + sandbox_err
            : sandbox_err;
        return false;
    }
}

bool DapServer::matches_hit_condition(const std::string& hit_condition,
                                      int hit_count,
                                      bool& out_match,
                                      std::string& out_error) {
    out_match = true;
    out_error.clear();

    const std::string text = trim_copy(hit_condition);
    if (text.empty()) return true;

    auto require_positive = [&](int64_t n) -> bool {
        if (n <= 0) {
            out_error = "value must be > 0";
            return false;
        }
        return true;
    };

    int64_t n = 0;
    if (try_parse_i64(text, n)) {
        if (!require_positive(n)) return false;
        out_match = (hit_count == n);
        return true;
    }

    if (text.rfind(">=", 0) == 0) {
        if (!try_parse_i64(trim_copy(text.substr(2)), n)) {
            out_error = "could not parse integer after '>='";
            return false;
        }
        if (!require_positive(n)) return false;
        out_match = (hit_count >= n);
        return true;
    }

    if (text.rfind(">", 0) == 0) {
        if (!try_parse_i64(trim_copy(text.substr(1)), n)) {
            out_error = "could not parse integer after '>'";
            return false;
        }
        if (!require_positive(n)) return false;
        out_match = (hit_count > n);
        return true;
    }

    if (text.rfind("==", 0) == 0) {
        if (!try_parse_i64(trim_copy(text.substr(2)), n)) {
            out_error = "could not parse integer after '=='";
            return false;
        }
        if (!require_positive(n)) return false;
        out_match = (hit_count == n);
        return true;
    }

    if (text.rfind("%", 0) == 0) {
        if (!try_parse_i64(trim_copy(text.substr(1)), n)) {
            out_error = "could not parse integer after '%'";
            return false;
        }
        if (!require_positive(n)) return false;
        out_match = (hit_count % n) == 0;
        return true;
    }

    out_error = "expected one of: '<n>', '== <n>', '>= <n>', '> <n>', '% <n>'";
    return false;
}

std::string DapServer::render_logpoint_message(session::Driver& drv, const std::string& templ) {
    std::string out;
    out.reserve(templ.size() + 32);

    std::size_t i = 0;
    while (i < templ.size()) {
        if (templ[i] != '{') {
            out.push_back(templ[i]);
            ++i;
            continue;
        }

        const std::size_t close = templ.find('}', i + 1);
        if (close == std::string::npos) {
            out.append(templ.substr(i));
            break;
        }

        const std::string expr = trim_copy(templ.substr(i + 1, close - (i + 1)));
        uint64_t val = runtime::nanbox::Nil;
        if (!expr.empty() && is_identifier_expr(expr) && try_lookup_paused_name(drv, expr, val)) {
            out.append(drv.format_value(val));
        } else {
            out.push_back('{');
            out.append(expr);
            out.push_back('}');
        }
        i = close + 1;
    }

    return out;
}

bool DapServer::resolve_function_breakpoint(
    const std::string& name,
    uint32_t& out_file_id,
    uint32_t& out_line,
    std::string* out_message) {
    out_file_id = 0;
    out_line = 0;

    if (!driver_) {
        if (out_message) *out_message = "debugger not launched";
        return false;
    }
    if (name.empty()) {
        if (out_message) *out_message = "missing function name";
        return false;
    }

    struct Match {
        uint32_t file_id{0};
        uint32_t line{0};
        std::string full_name;
    };

    std::vector<Match> exact_matches;
    std::vector<Match> short_matches;

    const auto& globals = driver_->vm().globals();
    const auto& names = driver_->global_names();

    for (const auto& [slot, full_name] : names) {
        if (slot >= globals.size()) continue;

        const auto value = globals[slot];
        if (!runtime::nanbox::ops::is_boxed(value) ||
            runtime::nanbox::ops::tag(value) != runtime::nanbox::Tag::HeapObject) {
            continue;
        }

        auto* closure = driver_->heap().try_get_as<
            runtime::memory::heap::ObjectKind::Closure, runtime::types::Closure>(
                runtime::nanbox::ops::payload(value));
        if (!closure || !closure->func || closure->func->source_map.empty()) {
            continue;
        }

        const auto span = closure->func->source_map.front();
        if (span.file_id == 0 || span.start.line == 0) continue;

        Match match;
        match.file_id = span.file_id;
        match.line = span.start.line;
        match.full_name = full_name;

        if (full_name == name) {
            exact_matches.push_back(match);
        }

        const auto dot = full_name.rfind('.');
        const std::string short_name = (dot == std::string::npos)
            ? full_name
            : full_name.substr(dot + 1);
        if (short_name == name) {
            short_matches.push_back(match);
        }
    }

    auto apply_match = [&](const Match& m) {
        out_file_id = m.file_id;
        out_line = m.line;
    };

    if (!exact_matches.empty()) {
        apply_match(exact_matches.front());
        return true;
    }

    if (short_matches.size() == 1) {
        apply_match(short_matches.front());
        return true;
    }

    if (short_matches.size() > 1) {
        if (out_message) {
            std::ostringstream oss;
            oss << "ambiguous function breakpoint: " << name;
            const std::size_t max_names = 4;
            std::size_t count = 0;
            for (const auto& m : short_matches) {
                if (count == 0) {
                    oss << " (matches ";
                } else {
                    oss << ", ";
                }
                oss << m.full_name;
                ++count;
                if (count >= max_names) break;
            }
            if (short_matches.size() > max_names) {
                oss << ", ...";
            }
            if (count > 0) {
                oss << ')';
            }
            *out_message = oss.str();
        }
        return false;
    }

    if (out_message) *out_message = "function not found: " + name;
    return false;
}

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

    for (const auto& bp : pending_function_bps_) {
        uint32_t file_id = 0;
        uint32_t line = 0;
        if (resolve_function_breakpoint(bp.name, file_id, line, nullptr)) {
            all_locs.push_back({file_id, line});
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

        for (const auto& bp : pending_function_bps_) {
            uint32_t file_id = 0;
            uint32_t line = 0;
            if (resolve_function_breakpoint(bp.name, file_id, line, nullptr)) {
                verified.push_back({bp.id, static_cast<int>(line)});
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

    /// Per-kind statistics
    struct KindStat { int64_t count{0}; int64_t bytes{0}; };
    std::unordered_map<uint8_t, KindStat> kind_stats;
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
                auto k = static_cast<uint8_t>(entry.header.kind);
                kind_stats[k].count++;
                kind_stats[k].bytes += static_cast<int64_t>(entry.size);
            });
        } catch (const HeapSnapshotCancelled&) {
            if (out_cancelled) *out_cancelled = true;
            return json::object({});
        }
    }

    Array kinds_arr;
    if (opts.include_kinds) {
        std::vector<std::pair<uint8_t, KindStat>> rows;
        rows.reserve(kind_stats.size());
        for (const auto& kv : kind_stats) rows.push_back(kv);
        std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) {
                if (a.second.bytes != b.second.bytes) return a.second.bytes > b.second.bytes;
                if (a.second.count != b.second.count) return a.second.count > b.second.count;
                return a.first < b.first;
            });
        if (opts.max_kind_rows > 0
            && static_cast<int64_t>(rows.size()) > opts.max_kind_rows) {
            rows.resize(static_cast<std::size_t>(opts.max_kind_rows));
            truncated = true;
        }
        for (const auto& [k, stat] : rows) {
            kinds_arr.push_back(json::object({
                {"kind",  Value(std::string(to_string(static_cast<ObjectKind>(k))))},
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

    return json::object({
        {"totalBytes",      Value(static_cast<int64_t>(heap.total_bytes()))},
        {"softLimit",       Value(static_cast<int64_t>(heap.soft_limit()))},
        {"kinds",           Value(std::move(kinds_arr))},
        {"roots",           Value(std::move(roots_arr))},
        {"consPool",        Value(std::move(cons_pool_obj))},
        {"truncated",       Value(truncated)},
        {"scannedObjects",  Value(scanned_objects)},
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

/**
 * Compound variable expansion helpers
 */

int DapServer::alloc_compound_ref(uint64_t val, int dap_thread_id) {
    int ref = next_compound_ref_++;
    compound_refs_[ref] = CompoundRef{val, dap_thread_id};
    return ref;
}

void DapServer::clear_compound_refs() {
    compound_refs_.clear();
    next_compound_ref_ = COMPOUND_REF_BASE;
}

bool DapServer::is_compound_value(uint64_t val) const {
    using namespace runtime::nanbox;
    if (!ops::is_boxed(val) || ops::tag(val) != Tag::HeapObject) return false;
    if (!driver_) return false;
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

Value DapServer::make_variable_json(session::Driver& drv, int dap_thread_id,
                                    const std::string& name, uint64_t val) {
    using namespace runtime::nanbox;
    using namespace runtime::memory::heap;
    using namespace runtime::types;

    int var_ref = 0;
    int64_t indexed_count = 0;
    int64_t named_count   = 0;
    /// is_compound_value() probes the *parent* heap; for child threads we
    /// re-probe against `drv.heap()` directly to determine expandability.
    auto& heap = drv.heap();
    if (ops::is_boxed(val) && ops::tag(val) == Tag::HeapObject) {
        const auto pid = static_cast<ObjectId>(ops::payload(val));
        HeapEntry entry;
        if (heap.try_get(pid, entry)) {
            switch (entry.header.kind) {
                case ObjectKind::Vector:
                    if (auto* vec = heap.try_get_as<ObjectKind::Vector, Vector>(pid)) {
                        var_ref = alloc_compound_ref(val, dap_thread_id);
                        indexed_count = static_cast<int64_t>(vec->elements.size());
                    }
                    break;
                case ObjectKind::Cons:
                    var_ref = alloc_compound_ref(val, dap_thread_id);
                    named_count = 2;
                    break;
                case ObjectKind::Closure:
                    if (auto* cl = heap.try_get_as<ObjectKind::Closure, Closure>(pid)) {
                        var_ref = alloc_compound_ref(val, dap_thread_id);
                        named_count = static_cast<int64_t>(cl->upvals.size()) + (cl->func ? 1 : 0);
                    }
                    break;
#ifdef ETA_HAS_NNG
                case ObjectKind::NngSocket:
                    var_ref = alloc_compound_ref(val, dap_thread_id);
                    break;
#endif
                default: break;
            }
        }
    }
    auto obj = json::object({
        {"name",               Value(name)},
        {"value",              Value(drv.format_value(val))},
        {"variablesReference", Value(static_cast<int64_t>(var_ref))},
    });
    if (indexed_count > 0) {
        obj.as_object().insert_or_assign("indexedVariables", Value(indexed_count));
    }
    if (named_count > 0) {
        obj.as_object().insert_or_assign("namedVariables", Value(named_count));
    }
    return obj;
}

Array DapServer::expand_compound(session::Driver& drv, int dap_thread_id,
                                 uint64_t val, int start, int count) {
    using namespace runtime::nanbox;
    using namespace runtime::memory::heap;
    using namespace runtime::types;

    Array children;
    auto& heap = drv.heap();

    if (auto* cons = heap.try_get_as<ObjectKind::Cons, Cons>(
            static_cast<ObjectId>(ops::payload(val)))) {
        children.push_back(make_variable_json(drv, dap_thread_id, "car", cons->car));
        children.push_back(make_variable_json(drv, dap_thread_id, "cdr", cons->cdr));
        return children;
    }

    if (auto* vec = heap.try_get_as<ObjectKind::Vector, Vector>(
            static_cast<ObjectId>(ops::payload(val)))) {
        const std::size_t total = vec->elements.size();
        std::size_t s = (start > 0) ? static_cast<std::size_t>(start) : 0;
        if (s > total) s = total;
        std::size_t end;
        if (count > 0) {
            end = s + static_cast<std::size_t>(count);
            if (end > total) end = total;
        } else if (start > 0) {
            end = total;
        } else {
            end = (std::min)(total, std::size_t{200});
        }

        children.reserve(end - s);
        for (std::size_t i = s; i < end; ++i) {
            children.push_back(make_variable_json(drv, dap_thread_id,
                "[" + std::to_string(i) + "]", vec->elements[i]));
        }
        if (start <= 0 && count <= 0 && total > 200) {
            children.push_back(json::object({
                {"name",  "..."},
                {"value", Value("(" + std::to_string(total - 200) + " more)")},
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
            children.push_back(make_variable_json(drv, dap_thread_id, uname, cl->upvals[i]));
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
    dap_debug_log(
        "on_thread_stopped entry tid=" + std::to_string(dap_thread_id)
        + " reason=" + reason_str
    );

    bool should_emit_stopped = true;
    bool is_main = (dap_thread_id == MAIN_THREAD_ID);
    std::vector<std::string> deferred_output;

    dap_debug_log("on_thread_stopped acquiring vm_mutex");
    {
        std::lock_guard<std::mutex> lk(vm_mutex_);
        dap_debug_log("on_thread_stopped acquired vm_mutex");

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
                dap_debug_log("on_thread_stopped auto-resume (logpoint/condition filtered stop)");
                th->vm->resume();
            }
        }
    }
    dap_debug_log(
        "on_thread_stopped released vm_mutex should_emit_stopped="
        + std::string(should_emit_stopped ? "true" : "false")
    );

    for (const auto& text : deferred_output) {
        dap_debug_log("on_thread_stopped flushing deferred output line");
        send_event("output", json::object({
            {"category", "console"},
            {"output", text},
        }));
    }

    if (!should_emit_stopped) {
        dap_debug_log("on_thread_stopped exit without stopped event");
        return;
    }

    dap_debug_log("on_thread_stopped sending stopped event");
    send_event("stopped", json::object({
        {"reason",            reason_str},
        {"threadId",          Value(static_cast<int64_t>(dap_thread_id))},
        {"allThreadsStopped", Value(is_main)},
    }));
    dap_debug_log("on_thread_stopped stopped event sent");
}

void DapServer::install_pending_breakpoints_on_locked(session::Driver& drv) {
    std::vector<runtime::vm::BreakLocation> all_locs;
    for (const auto& [path, bps] : pending_bps_) {
        uint32_t file_id = drv.file_id_for_path(path);
        if (file_id == 0) continue;
        for (const auto& bp : bps) {
            all_locs.push_back({file_id, static_cast<uint32_t>(bp.line)});
        }
    }
    drv.vm().set_breakpoints(std::move(all_locs));
}

Value DapServer::source_json_for(session::Driver& drv, uint32_t file_id) {
    if (file_id == 0) return Value{};
    if (auto* path = drv.path_for_file_id(file_id)) {
        return json::object({
            {"name", path->filename().string()},
            {"path", path->string()},
        });
    }
    return Value{};
}

} ///< namespace eta::dap



