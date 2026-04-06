#include "dap_server.h"
#include "dap_io.h"

#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>

// Eta interpreter
#include "eta/interpreter/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/runtime/port.h"
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/value_formatter.h"
#include "eta/diagnostic/diagnostic.h"

namespace eta::dap {

namespace fs = std::filesystem;
using namespace json;

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

/// Normalise a source-file path to a stable key that matches the normalisation
/// used by Driver::file_id_for_path / Driver::ensure_file_id.
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

// ============================================================================
// Construction
// ============================================================================

DapServer::DapServer() : in_(std::cin), out_(std::cout) {}

DapServer::DapServer(std::istream& in, std::ostream& out) : in_(in), out_(out) {}

// ============================================================================
// Destruction
// ============================================================================

DapServer::~DapServer() {
    // If the VM thread is still running, ask it to stop and join.
    if (driver_) {
        driver_->vm().request_pause();
    }
    if (vm_thread_.joinable()) {
        vm_thread_.join();
    }
}

// ============================================================================
// Transport
// ============================================================================

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

// ============================================================================
// Main loop
// ============================================================================

void DapServer::run() {
    while (running_) {
        auto msg_str = read_message(in_);
        if (!msg_str) break;

        try {
            auto msg = json::parse(*msg_str);
            dispatch(msg);
        } catch (const std::exception& e) {
            // Malformed message – log to stderr and continue
            std::cerr << "[eta_dap] parse error: " << e.what() << "\n";
        }
    }

    // Join the VM thread if still running
    if (vm_thread_.joinable()) {
        vm_thread_.join();
    }
}

// ============================================================================
// Dispatch
// ============================================================================

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
    else if (*cmd == "disconnect")          handle_disconnect(id, args);
    else {
        // Unknown command – send empty success response to keep VS Code happy
        send_response(id, json::object({}));
    }
}

// ============================================================================
// initialize
// ============================================================================

void DapServer::handle_initialize(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({
        {"supportsConfigurationDoneRequest",    true},
        {"supportsFunctionBreakpoints",         false},
        {"supportsConditionalBreakpoints",      false},
        {"supportsSetVariable",                 false},
        {"supportsRestartRequest",              false},
        {"supportsTerminateRequest",            true},
        {"supportsEvaluateForHovers",           true},
        {"supportsStepBack",                    false},
        {"supportsGotoTargetsRequest",          false},
        {"supportsBreakpointLocationsRequest",  false},
    }));
    // "initialized" is sent from handle_launch (see below), per the DAP spec:
    //   the adapter should send "initialized" AFTER processing "launch"/"attach"
    //   so VS Code fires setBreakpoints/configurationDone only after script_path_
    //   is known.  Sending it here caused 0 breakpoints every time.
}

// ============================================================================
// launch
// ============================================================================

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

    // Per DAP spec: send "initialized" AFTER the launch response so VS Code
    // sends setBreakpoints and configurationDone AFTER script_path_ is set.
    send_event("initialized", json::object({}));

    send_event("output", json::object({
        {"category", "console"},
        {"output", "[eta_dap] Launch: " + script_path_.string() + "\n"},
    }));
    // The VM is actually started on configurationDone.
}

// ============================================================================
// setBreakpoints
// ============================================================================

void DapServer::handle_set_breakpoints(const Value& id, const Value& args) {
    Array result_bps;

    auto source_obj  = args["source"];
    auto path_val    = source_obj.get_string("path");
    if (!path_val) {
        send_response(id, json::object({{"breakpoints", json::array({})}}));
        return;
    }

    // Normalise so it matches the driver's canon_path_key lookup
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
                {"verified", false},   // updated via "breakpoint" event once installed
                {"id",       Value(static_cast<int64_t>(bp_id))},
                {"line",     Value(static_cast<int64_t>(line))},
            }));
        }
    }

    // Diagnostic: always log what was received so path mismatches are visible
    {
        std::ostringstream msg;
        msg << "[eta_dap] setBreakpoints: " << result_bps.size()
            << " bp(s) for \"" << canon_path << "\"\n";
        send_event("output", json::object({{"category", "console"}, {"output", msg.str()}}));
    }

    // If the VM is already running, push immediately and notify VS Code
    if (driver_) {
        {
            std::lock_guard<std::mutex> lk(vm_mutex_);
            install_pending_breakpoints();
        }
        notify_breakpoints_verified();
        // Mark returned breakpoints as verified in the response too
        for (auto& bp_val : result_bps) {
            if (bp_val.is_object()) bp_val.as_object()["verified"] = Value(true);
        }
    }

    send_response(id, json::object({{"breakpoints", Value(std::move(result_bps))}}));
}

// ============================================================================
// setExceptionBreakpoints
// ============================================================================

void DapServer::handle_set_exception_breakpoints(const Value& id, const Value& /*args*/) {
    // We don't (yet) support exception breakpoints – just acknowledge.
    send_response(id, json::object({}));
}

// ============================================================================
// configurationDone — start the VM
// ============================================================================

void DapServer::handle_configuration_done(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({}));

    if (!launched_) return; // no launch request yet (shouldn't happen)

    // Determine the stdlib/module path (same logic as etai and eta_lsp)
    std::vector<fs::path> search_dirs;
    {
        const char* env = std::getenv("ETA_MODULE_PATH");
        if (env && env[0] != '\0') {
            std::string_view sv(env);
#ifdef _WIN32
            constexpr char sep = ';';
#else
            constexpr char sep = ':';
#endif
            std::size_t start = 0;
            while (start < sv.size()) {
                auto pos = sv.find(sep, start);
                if (pos == std::string_view::npos) pos = sv.size();
                auto p = std::string(sv.substr(start, pos - start));
                if (!p.empty()) search_dirs.emplace_back(p);
                start = pos + 1;
            }
        }

        // Bundled stdlib: <exe_dir>/../stdlib
        std::error_code ec;
#ifdef _WIN32
        wchar_t buf[4096];
        DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
        if (len > 0 && len < static_cast<DWORD>(std::size(buf))) {
            auto stdlib = fs::path(buf).parent_path().parent_path() / "stdlib";
            if (fs::is_directory(stdlib, ec)) search_dirs.push_back(stdlib);
        }
#else
        auto exe = fs::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            auto stdlib = exe.parent_path().parent_path() / "stdlib";
            if (fs::is_directory(stdlib, ec)) search_dirs.push_back(stdlib);
        }
#endif
        // Also try sibling of the script file
        auto script_dir = script_path_.parent_path();
        if (!script_dir.empty()) search_dirs.push_back(script_dir);
    }

    // Deduplicate: both ETA_MODULE_PATH and the <exe>/../stdlib detection can
    // find the same directory; keep the first occurrence of each canonical path.
    {
        std::vector<fs::path> unique_dirs;
        std::unordered_set<std::string> seen;
        for (auto& d : search_dirs) {
            std::error_code ec2;
            std::string key = fs::weakly_canonical(d, ec2).string();
#ifdef _WIN32
            for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
            if (seen.insert(key).second) unique_dirs.push_back(std::move(d));
        }
        search_dirs = std::move(unique_dirs);
    }

    // Log the module search path so the user can diagnose "module not found"
    {
        std::ostringstream msg;
        msg << "[eta_dap] Module search dirs:\n";
        if (search_dirs.empty()) {
            msg << "  (none — prelude will not load; set eta.lsp.modulePath in VS Code settings)\n";
        } else {
            for (const auto& d : search_dirs) msg << "  " << d.string() << "\n";
        }
        send_event("output", json::object({{"category", "console"}, {"output", msg.str()}}));
    }

    auto resolver = interpreter::ModulePathResolver(search_dirs);

    // Build the driver on the DAP thread (it will be moved-to below)
    auto drv = std::make_unique<interpreter::Driver>(std::move(resolver));

    // Register stop callback BEFORE loading prelude so the hook is in place
    drv->vm().set_stop_callback([this](const runtime::vm::StopEvent& ev) {
        using runtime::vm::StopReason;

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

    // ── Redirect script stdout/stderr away from the protocol pipe ─────────────
    // Do this on the LOCAL drv BEFORE making it visible via driver_.
    // Without this, any script that calls (display ...) or (newline) would write
    // to std::cout, corrupting the Content-Length framed DAP stream and causing
    // VS Code to report a "read error".  We install CallbackPorts that forward
    // the text as DAP "output" events instead.
    drv->set_output_port(std::make_shared<runtime::CallbackPort>(
        [this](const std::string& text) {
            send_event("output", json::object({{"category", "stdout"}, {"output", text}}));
        }));
    drv->set_error_port(std::make_shared<runtime::CallbackPort>(
        [this](const std::string& text) {
            send_event("output", json::object({{"category", "stderr"}, {"output", text}}));
        }));

    // ── Pre-register the script file ID so breakpoints fire on first run ──────
    // We allocate the file_id NOW (before the VM thread starts loading any file).
    // When run_file() is later called with the same (normalised) path, Driver
    // reuses this id — so the breakpoints we install below will be active
    // inside vm_.execute().
    drv->ensure_file_id(script_path_);

    // ── Atomically publish driver_ and install breakpoints under the lock ─────
    // All setup is done on the local drv above; only now do we make it visible
    // to other DAP-thread handlers (evaluate, setBreakpoints, etc.).
    {
        std::lock_guard<std::mutex> lk(vm_mutex_);
        driver_ = std::move(drv);
        install_pending_breakpoints();
    }
    // Send "breakpoint" changed events for all newly-verified breakpoints.
    // Must be called WITHOUT vm_mutex_ held (uses output_mutex_ internally).
    notify_breakpoints_verified();

    {
        std::size_t bp_count = 0;
        for (const auto& [p, bps] : pending_bps_) bp_count += bps.size();
        std::ostringstream msg;
        msg << "[eta_dap] " << bp_count << " breakpoint(s) installed; "
            << "script file_id = " << driver_->file_id_for_path(script_path_.string()) << "\n";
        send_event("output", json::object({{"category", "console"}, {"output", msg.str()}}));
    }

    // If stopOnEntry, request a pause immediately (before any code runs)
    if (stop_on_entry_) {
        driver_->vm().request_pause();
    }

    // Launch the VM on a background thread
    vm_thread_ = std::thread([this]() {
        auto* drv = driver_.get();

        // ── Load prelude ─────────────────────────────────────────────────────
        auto pr = drv->load_prelude();
        if (!pr.found) {
            send_event("output", json::object({
                {"category", "important"},
                {"output",
                    "[eta_dap] Warning: prelude.eta not found — std.core, std.io, std.prelude etc. unavailable.\n"
                    "[eta_dap] Set 'eta.lsp.modulePath' in VS Code settings, or set ETA_MODULE_PATH.\n"},
            }));
        } else if (!pr.loaded) {
            const auto& diags = drv->diagnostics().diagnostics();
            for (const auto& d : diags) {
                std::ostringstream msg;
                diagnostic::format_diagnostic(msg, d, /*use_color=*/false);
                send_event("output", json::object({{"category", "stderr"}, {"output", msg.str() + "\n"}}));
            }
        } else {
            send_event("output", json::object({
                {"category", "console"},
                {"output", "[eta_dap] Prelude loaded: " + pr.path.string() + "\n"},
            }));
        }

        // Re-install now that prelude file IDs are known, so breakpoints in
        // library files are active before the script starts executing.
        {
            std::lock_guard<std::mutex> lk(vm_mutex_);
            install_pending_breakpoints();
        }
        // Notify VS Code about any breakpoints that became verified after prelude load
        notify_breakpoints_verified();

        // ── Execute the script ────────────────────────────────────────────────
        bool ok = drv->run_file(script_path_);

        // ── Signal IDE ───────────────────────────────────────────────────────
        if (ok) {
            send_event("terminated", json::object({}));
        } else {
            // Forward the real compiler/linker/runtime diagnostics — not just
            // a generic "Script failed" message.
            const auto& diags = drv->diagnostics().diagnostics();
            if (diags.empty()) {
                send_event("output", json::object({
                    {"category", "stderr"},
                    {"output", "[eta_dap] Script failed: " + script_path_.filename().string() + "\n"},
                }));
            } else {
                for (const auto& d : diags) {
                    std::ostringstream msg;
                    diagnostic::format_diagnostic(msg, d, /*use_color=*/false);
                    send_event("output", json::object({{"category", "stderr"}, {"output", msg.str() + "\n"}}));
                }
            }
            send_event("terminated", json::object({}));
        }
    });
}

// ============================================================================
// threads
// ============================================================================

void DapServer::handle_threads(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({
        {"threads", json::array({
            json::object({{"id", 1}, {"name", "main"}}),
        })},
    }));
}

// ============================================================================
// stackTrace
// ============================================================================

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

// ============================================================================
// scopes
// ============================================================================

void DapServer::handle_scopes(const Value& id, const Value& args) {
    // frame_id comes from the request args, not from VM state — no lock needed.
    auto frame_id_opt = args.get_int("frameId");
    int frame_id = frame_id_opt ? static_cast<int>(*frame_id_opt) : 0;

    send_response(id, json::object({
        {"scopes", json::array({
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
        })},
    }));
}

// ============================================================================
// variables
// ============================================================================

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

    int ref        = static_cast<int>(*ref_opt);
    int frame_idx  = decode_var_ref_frame(ref);
    int scope      = decode_var_ref_scope(ref);

    auto entries = (scope == 0)
                   ? driver_->vm().get_locals(static_cast<std::size_t>(frame_idx))
                   : driver_->vm().get_upvalues(static_cast<std::size_t>(frame_idx));

    Array vars;
    for (const auto& e : entries) {
        vars.push_back(json::object({
            {"name",               Value(e.name)},
            {"value",              Value(driver_->format_value(e.value))},
            {"variablesReference", 0},
        }));
    }

    send_response(id, json::object({{"variables", Value(std::move(vars))}}));
}

// ============================================================================
// continue / next / stepIn / stepOut / pause
// ============================================================================

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

// ============================================================================
// evaluate
// ============================================================================

void DapServer::handle_evaluate(const Value& id, const Value& args) {
    auto expr_opt = args.get_string("expression");
    if (!expr_opt) {
        send_response(id, json::object({{"result", "<not available>"}, {"variablesReference", 0}}));
        return;
    }

    // Lock vm_mutex_ for the entire evaluation to prevent concurrent VM access.
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (!driver_) {
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

// ============================================================================
// disconnect
// ============================================================================

void DapServer::handle_disconnect(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({}));
    running_ = false;

    // Wake up the VM if it's paused so the vm_thread can exit.
    // Lock vm_mutex_ so driver_ isn't torn out mid-access.
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (driver_) {
        driver_->vm().resume();
    }
}

// ============================================================================
// Helpers
// ============================================================================

void DapServer::install_pending_breakpoints() {
    if (!driver_) return;

    std::vector<runtime::vm::BreakLocation> all_locs;

    for (const auto& [path, bps] : pending_bps_) {
        uint32_t file_id = driver_->file_id_for_path(path);
        if (file_id == 0) continue; // file not loaded yet — will be re-applied after load

        for (const auto& bp : bps) {
            all_locs.push_back({file_id, static_cast<uint32_t>(bp.line)});
        }
    }

    driver_->vm().set_breakpoints(std::move(all_locs));
}

void DapServer::notify_breakpoints_verified() {
    // Collect verified breakpoint data under the lock, then send events after.
    // This avoids calling send_event (→ output_mutex_) while holding vm_mutex_.
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

    // Send "breakpoint" changed events so VS Code shows solid red dots.
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


} // namespace eta::dap



