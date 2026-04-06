#include "dap_server.h"
#include "dap_io.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

// Eta interpreter
#include "eta/interpreter/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/value_formatter.h"

namespace eta::dap {

namespace fs = std::filesystem;
using namespace json;

// ============================================================================
// Construction / Destruction
// ============================================================================

DapServer::DapServer() = default;

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
    write_message(json::to_string(msg));
}

void DapServer::send_response(const Value& id, const Value& body) {
    send(json::object({
        {"seq",         Value(next_seq_++)},
        {"type",        "response"},
        {"request_seq", id},
        {"success",     true},
        {"body",        body},
    }));
}

void DapServer::send_error_response(const Value& id, int code, const std::string& msg) {
    send(json::object({
        {"seq",         Value(next_seq_++)},
        {"type",        "response"},
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
        auto msg_str = read_message();
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

    send_event("initialized", json::object({}));
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

    std::string canon_path = *path_val;
    pending_bps_.erase(canon_path);

    if (args.has("breakpoints") && args["breakpoints"].is_array()) {
        for (const auto& bp : args["breakpoints"].as_array()) {
            auto line_opt = bp.get_int("line");
            if (!line_opt) continue;
            int line = static_cast<int>(*line_opt);

            pending_bps_[canon_path].push_back({line});

            result_bps.push_back(json::object({
                {"verified", false},   // verified=true once we confirm the location exists
                {"line",     Value(static_cast<int64_t>(line))},
            }));
        }
    }

    // If the VM is already running, push immediately
    if (driver_) {
        install_pending_breakpoints();
        // Mark all returned breakpoints as verified now
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
        if (len > 0) {
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

    {
        std::lock_guard<std::mutex> lk(vm_mutex_);
        driver_ = std::move(drv);
    }

    // Install any breakpoints that arrived before the VM was started
    install_pending_breakpoints();

    // If stopOnEntry, request a pause immediately (before any code runs)
    if (stop_on_entry_) {
        driver_->vm().request_pause();
    }

    // Launch the VM on a background thread
    vm_thread_ = std::thread([this]() {
        auto* drv = driver_.get();

        // Load prelude
        drv->load_prelude();

        // Execute the script
        bool ok = drv->run_file(script_path_);

        // Signal termination to the IDE
        if (ok) {
            send_event("terminated", json::object({}));
        } else {
            // Collect and forward diagnostics as an output event
            std::ostringstream err_msg;
            err_msg << "Script failed: " << script_path_.filename().string();
            send_event("output", json::object({
                {"category", "stderr"},
                {"output",   err_msg.str() + "\n"},
            }));
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
    if (driver_) driver_->vm().resume();
}

void DapServer::handle_next(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({}));
    if (driver_) driver_->vm().step_over();
}

void DapServer::handle_step_in(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({}));
    if (driver_) driver_->vm().step_in();
}

void DapServer::handle_step_out(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({}));
    if (driver_) driver_->vm().step_out();
}

void DapServer::handle_pause(const Value& id, const Value& /*args*/) {
    send_response(id, json::object({}));
    if (driver_) driver_->vm().request_pause();
}

// ============================================================================
// evaluate
// ============================================================================

void DapServer::handle_evaluate(const Value& id, const Value& args) {
    auto expr_opt = args.get_string("expression");
    if (!expr_opt || !driver_) {
        send_response(id, json::object({{"result", "<not available>"}, {"variablesReference", 0}}));
        return;
    }

    // Expression evaluation while paused: run in the driver on the DAP thread.
    // We lock vm_mutex_ to prevent the VM thread from resuming concurrently.
    std::lock_guard<std::mutex> lk(vm_mutex_);

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

    // Wake up the VM if it's paused so the vm_thread can exit
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
        if (file_id == 0) continue; // file not loaded yet — breakpoints will be re-applied after load

        for (const auto& bp : bps) {
            all_locs.push_back({file_id, static_cast<uint32_t>(bp.line)});
        }
    }

    driver_->vm().set_breakpoints(std::move(all_locs));
}

std::string DapServer::uri_to_path(const std::string& uri) {
    // Strip file:// prefix and percent-decode
    std::string path = uri;
    if (path.substr(0, 7) == "file://") {
        path = path.substr(7);
#ifdef _WIN32
        // Remove leading slash before drive letter: /C:/... -> C:/...
        if (path.size() >= 3 && path[0] == '/' && path[2] == ':')
            path = path.substr(1);
#endif
    }
    // Basic percent-decoding
    std::string result;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size()) {
            std::string hex = path.substr(i + 1, 2);
            result += static_cast<char>(std::stoul(hex, nullptr, 16));
            i += 2;
        } else {
            result += path[i];
        }
    }
    return result;
}

std::string DapServer::path_to_uri(const std::string& path) {
    std::string uri = "file://";
#ifdef _WIN32
    uri += "/";
    for (char c : path) {
        if (c == '\\') uri += '/';
        else uri += c;
    }
#else
    uri += path;
#endif
    return uri;
}

} // namespace eta::dap

