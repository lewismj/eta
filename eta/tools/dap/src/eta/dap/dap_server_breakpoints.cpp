/// DAP breakpoint requests: source, function and exception breakpoints, conditions, logpoints.
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
} ///< namespace eta::dap

