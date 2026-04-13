#pragma once
// DAP (Debug Adapter Protocol) server for eta scripts.
// Runs as a separate process launched by the VS Code extension via DebugAdapterExecutable.
// Communicates over stdin/stdout using Content-Length framed JSON-RPC (same as LSP).

#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "eta/util/json.h"

namespace eta::interpreter { class Driver; }

namespace eta::dap {

using eta::json::Value;
using eta::json::Object;
using eta::json::Array;

class DapServer {
public:
    /// Production constructor: communicates over the process stdin/stdout.
    DapServer();

    /// Testable constructor: communicates over the supplied streams.
    /// Useful for unit tests that inject std::istringstream / std::ostringstream.
    explicit DapServer(std::istream& in, std::ostream& out);

    ~DapServer();

    /// Main loop: reads DAP requests from stdin, writes responses/events to stdout.
    /// Blocks until the "disconnect" request is received or stdin closes.
    void run();

    // Variable reference packing: encode (frame_index, scope) into a single int.
    // scope 0 = locals, scope 1 = upvalues
    // +1 offset ensures the result is always >= 1: DAP treats variablesReference == 0
    // as "not expandable", so frame 0 / scope 0 (top-frame locals) would silently
    // suppress the variables request without this offset.
    static int encode_var_ref(int frame, int scope) { return ((frame << 8) | (scope & 0xFF)) + 1; }
    static int decode_var_ref_frame(int ref) { return (ref - 1) >> 8; }
    static int decode_var_ref_scope(int ref) { return (ref - 1) & 0xFF; }

    // Compound variable references start at this value.
    static constexpr int COMPOUND_REF_BASE = 10000;

private:
    // Injected I/O
    std::istream& in_;
    std::ostream& out_;

    // State
    bool running_{true};
    int  next_seq_{1};

    // Script to debug (set by "launch" request)
    std::filesystem::path script_path_;

    // Breakpoints pending before VM is running: path → [(line, id)]
    struct PendingBp { int line; int id; };
    std::unordered_map<std::string, std::vector<PendingBp>> pending_bps_;
    int next_bp_id_{1};  ///< monotonically increasing breakpoint ID assigned by adapter

    // Whether to stop on first instruction (stopOnEntry)
    bool stop_on_entry_{false};

    // Whether exception breakpoints are enabled (set via setExceptionBreakpoints)
    bool exception_breakpoints_enabled_{false};

    // Whether "launch" has been requested (i.e., VM thread is running/should start)
    bool launched_{false};

    // Command name of the request currently being dispatched (used by send_response).
    std::string current_command_;

    // The driver and its execution thread
    std::unique_ptr<interpreter::Driver> driver_;
    std::thread vm_thread_;
    std::mutex  vm_mutex_;    // guards driver_ / VM state access from DAP thread
    std::mutex  output_mutex_; // serialises send() calls from DAP + VM threads

    // Transport
    void send(const Value& msg);
    void send_response(const Value& id, const Value& body);
    void send_error_response(const Value& id, int code, const std::string& msg);
    void send_event(const std::string& event_name, const Value& body);

    // Dispatch
    void dispatch(const Value& msg);

    // DAP request handlers
    void handle_initialize(const Value& id, const Value& args);
    void handle_launch(const Value& id, const Value& args);
    void handle_set_breakpoints(const Value& id, const Value& args);
    void handle_set_exception_breakpoints(const Value& id, const Value& args);
    void handle_configuration_done(const Value& id, const Value& args);
    void handle_threads(const Value& id, const Value& args);
    void handle_stack_trace(const Value& id, const Value& args);
    void handle_scopes(const Value& id, const Value& args);
    void handle_variables(const Value& id, const Value& args);
    void handle_continue(const Value& id, const Value& args);
    void handle_next(const Value& id, const Value& args);
    void handle_step_in(const Value& id, const Value& args);
    void handle_step_out(const Value& id, const Value& args);
    void handle_pause(const Value& id, const Value& args);
    void handle_evaluate(const Value& id, const Value& args);
    void handle_terminate(const Value& id, const Value& args);
    void handle_completions(const Value& id, const Value& args);
    void handle_disconnect(const Value& id, const Value& args);

    // Custom requests
    void handle_heap_inspector(const Value& id, const Value& args);
    void handle_inspect_object(const Value& id, const Value& args);
    void handle_disassemble(const Value& id, const Value& args);
    void handle_child_processes(const Value& id, const Value& args);

    // Helpers
    /// Build a JSON heap snapshot from the paused VM. Caller must hold vm_mutex_.
    Value build_heap_snapshot();

    /// Apply pending_bps_ to the running VM. Caller must hold vm_mutex_.
    void install_pending_breakpoints();

    /// Send DAP "breakpoint" changed events for every pending breakpoint whose
    /// file_id can now be resolved.  Must be called WITHOUT vm_mutex_ held.
    void notify_breakpoints_verified();

    // Compound variable expansion
    // References >= COMPOUND_REF_BASE are compound value expansions (cons, vector,
    // closure) stored in compound_refs_.  Cleared on each stop event.
    int next_compound_ref_{COMPOUND_REF_BASE};
    std::unordered_map<int, uint64_t> compound_refs_;  // ref → NaN-boxed LispVal

    /// Allocate a compound variablesReference for a NaN-boxed value.
    int alloc_compound_ref(uint64_t val);
    /// Clear compound refs (called on each stop/continue cycle).
    void clear_compound_refs();
    /// Check if a value is a compound type that can be expanded.
    bool is_compound_value(uint64_t val) const;
    /// Build a variable JSON object, assigning a compound ref if expandable.
    Value make_variable_json(const std::string& name, uint64_t val);
    /// Expand a compound value into child variables.
    Array expand_compound(uint64_t val);

    /// Derive the Eta module name (e.g. "composition", "std.io") that owns
    /// the function executing in the given frame index (0 = innermost).
    /// Must be called with vm_mutex_ held.
    std::string current_module_from_frame(std::size_t frame_idx);
};

} // namespace eta::dap

