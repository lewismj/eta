#include "eta/jupyter/eta_interpreter.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string_view>

#include "eta/interpreter/module_path.h"
#include "eta/interpreter/repl_complete.h"
#include "eta/jupyter/comm/actors_comm.h"
#include "eta/jupyter/comm/dag_comm.h"
#include "eta/jupyter/comm/disasm_comm.h"
#include "eta/jupyter/comm/heap_comm.h"
#include "eta/jupyter/comm/tensor_comm.h"
#include "eta/jupyter/display.h"
#include "eta/jupyter/magics.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/vm/disassembler.h"
#include "xeus/xhelper.hpp"

namespace eta::jupyter {

namespace {

[[nodiscard]] std::string error_name_for_code(eta::diagnostic::DiagnosticCode code) {
    using Code = eta::diagnostic::DiagnosticCode;
    switch (code) {
        case Code::TypeError:             return "TypeError";
        case Code::InvalidArity:          return "InvalidArity";
        case Code::UndefinedGlobal:       return "UndefinedGlobal";
        case Code::UserError:             return "UserError";
        case Code::StackOverflow:         return "StackOverflow";
        case Code::StackUnderflow:        return "StackUnderflow";
        case Code::FrameOverflow:         return "FrameOverflow";
        case Code::InvalidInstruction:    return "InvalidInstruction";
        case Code::NotImplemented:        return "NotImplemented";
        case Code::HeapAllocationFailed:  return "HeapAllocationFailed";
        case Code::InternTableFull:       return "InternTableFull";
        case Code::InternalError:         return "InternalError";
        default: break;
    }

    const std::string_view phase = eta::diagnostic::phase_for_code(code);
    if (phase == "lexer" || phase == "parser" || phase == "expander") {
        return "SyntaxError";
    }
    if (phase == "semantic" || phase == "linker") {
        return "CompileError";
    }
    if (phase == "runtime") {
        return "RuntimeError";
    }
    return "Error";
}

void trim_trailing_newlines(std::string& text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
}

[[nodiscard]] std::string trim_copy(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

[[nodiscard]] std::string eta_string_literal(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2u);
    out.push_back('"');
    for (const char ch : text) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    out.push_back('"');
    return out;
}

[[nodiscard]] std::string html_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

[[nodiscard]] std::string value_type_name(eta::session::Driver& driver,
                                          eta::runtime::nanbox::LispVal value) {
    using eta::runtime::nanbox::Tag;
    using eta::runtime::nanbox::ops::is_boxed;
    using eta::runtime::nanbox::ops::payload;
    using eta::runtime::nanbox::ops::tag;

    if (value == eta::runtime::nanbox::Nil) return "Nil";
    if (value == eta::runtime::nanbox::True) return "Boolean";
    if (value == eta::runtime::nanbox::False) return "Boolean";
    if (!is_boxed(value)) return "Flonum";

    const auto t = tag(value);
    if (t == Tag::Fixnum) return "Fixnum";
    if (t == Tag::Char) return "Char";
    if (t == Tag::String) return "String";
    if (t == Tag::Symbol) return "Symbol";
    if (t == Tag::TapeRef) return "TapeRef";
    if (t != Tag::HeapObject) return "Value";

    eta::runtime::memory::heap::HeapEntry entry;
    const auto id = static_cast<eta::runtime::memory::heap::ObjectId>(payload(value));
    if (!driver.heap().try_get(id, entry)) return "HeapObject";
    return std::string(eta::runtime::memory::heap::to_string(entry.header.kind));
}

[[nodiscard]] std::optional<std::pair<std::string, std::string>> split_env_assignment(
    std::string_view arg) {
    const auto eq = arg.find('=');
    if (eq == std::string_view::npos) return std::nullopt;
    auto key = trim_copy(arg.substr(0, eq));
    auto value = std::string(arg.substr(eq + 1));
    if (key.empty()) return std::nullopt;
    return std::pair<std::string, std::string>(std::move(key), std::move(value));
}

[[nodiscard]] std::optional<std::string> disassemble_source(
    eta::session::Driver& driver,
    std::string_view source,
    std::string* error_out) {
    namespace fs = std::filesystem;
    if (error_out) error_out->clear();

    const std::string trimmed = trim_copy(source);
    if (trimmed.empty()) return std::string{};

    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    const auto module_name = "__jupyter_magic_disasm_" + std::to_string(stamp);
    const auto tmp_path = fs::temp_directory_path() / (module_name + ".eta");

    std::ofstream out(tmp_path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error_out) *error_out = "failed to create temporary source file";
        return std::nullopt;
    }
    out << "(module " << module_name << "\n"
        << "  (begin\n"
        << trimmed << "\n"
        << "  ))\n";
    out.close();

    eta::session::Driver tmp_driver(driver.resolver());
    (void)tmp_driver.load_prelude();
    auto compile = tmp_driver.compile_file(tmp_path);
    std::error_code ec;
    fs::remove(tmp_path, ec);
    if (!compile) {
        std::ostringstream diagnostics;
        tmp_driver.diagnostics().print_all(
            diagnostics, /*use_color=*/false, tmp_driver.file_resolver());
        if (error_out) {
            auto text = diagnostics.str();
            trim_trailing_newlines(text);
            *error_out = text.empty() ? "bytecode compilation failed" : text;
        }
        return std::nullopt;
    }

    eta::runtime::vm::Disassembler disasm(tmp_driver.heap(), tmp_driver.intern_table());
    std::ostringstream oss;
    for (std::uint32_t idx = compile->base_func_idx; idx < compile->end_func_idx; ++idx) {
        if (const auto* fn = tmp_driver.registry().get(idx)) {
            disasm.disassemble(*fn, oss);
        }
    }

    auto text = oss.str();
    if (text.empty()) {
        text = "; no emitted functions\n";
    }
    return text;
}

} // namespace

EtaInterpreter::EtaInterpreter() {
    const auto loaded_config = load_kernel_config();
    kernel_config_ = loaded_config.config;
    render_options_ = kernel_config_.display;

    for (const auto& warning : loaded_config.warnings) {
        std::cerr << "[eta-jupyter] " << warning << '\n';
    }

    auto resolver = eta::interpreter::ModulePathResolver::from_args_or_env_at(
        "", std::filesystem::current_path());
    driver_ = std::make_unique<eta::session::Driver>(std::move(resolver));
    if (driver_) {
        (void)driver_->load_prelude();
    }
}

void EtaInterpreter::request_interrupt() noexcept {
    if (driver_) {
        driver_->request_interrupt();
    }
}

void EtaInterpreter::configure_impl() {
    apply_startup_configuration();
    register_comm_targets();
}

void EtaInterpreter::execute_request_impl(send_reply_callback cb,
                                          int execution_counter,
                                          const std::string& code,
                                          xeus::execute_request_config config,
                                          nl::json /*user_expressions*/) {
    std::lock_guard<std::mutex> lk(driver_mu_);
    if (!driver_) {
        const std::string msg = "Eta driver is not initialised";
        publish_execution_error("InternalError", msg, {msg});
        cb(nl::json{
            {"status", "error"},
            {"ename", "InternalError"},
            {"evalue", msg},
            {"traceback", nl::json::array({msg})},
        });
        return;
    }

    const auto previous_stdout = driver_->vm().current_output_port();
    const auto previous_stderr = driver_->vm().current_error_port();
    auto restore_ports = [this, previous_stdout, previous_stderr]() {
        driver_->vm().set_current_output_port(previous_stdout);
        driver_->vm().set_current_error_port(previous_stderr);
    };

    if (config.silent) {
        driver_->set_stream_sinks(
            [](std::string_view) {},
            [](std::string_view) {});
    } else {
        driver_->set_stream_sinks(
            [this](std::string_view chunk) {
                if (!chunk.empty()) publish_stream("stdout", std::string(chunk));
            },
            [this](std::string_view chunk) {
                if (!chunk.empty()) publish_stream("stderr", std::string(chunk));
            });
    }

    auto reply_error_from_driver = [&]() {
        std::string ename = "RuntimeError";
        std::string evalue;
        const auto& diagnostics = driver_->diagnostics().diagnostics();
        if (!diagnostics.empty()) {
            ename = error_name_for_code(diagnostics.front().code);
            evalue = diagnostics.front().message;
        }
        if (evalue.empty()) {
            evalue = diagnostics_to_text();
        }
        trim_trailing_newlines(evalue);
        if (evalue.empty()) {
            evalue = "evaluation failed";
        }
        auto traceback = traceback_lines(ename, evalue);
        publish_execution_error(ename, evalue, traceback);
        cb(nl::json{
            {"status", "error"},
            {"ename", ename},
            {"evalue", evalue},
            {"traceback", traceback},
        });
    };

    auto reply_magic_error = [&](const std::string& message) {
        std::string text = message;
        trim_trailing_newlines(text);
        if (text.empty()) {
            text = "invalid magic invocation";
        }
        publish_execution_error("MagicError", text, {"MagicError: " + text});
        cb(nl::json{
            {"status", "error"},
            {"ename", "MagicError"},
            {"evalue", text},
            {"traceback", nl::json::array({"MagicError: " + text})},
        });
    };

    std::string code_to_eval = code;
    bool emit_trace_widget = false;
    std::string trace_source;

    const auto magic = eta::jupyter::parse_magic(code);
    if (magic.kind != eta::jupyter::MagicKind::None) {
        auto arg = trim_copy(magic.args);
        switch (magic.name) {
            case eta::jupyter::MagicName::Time: {
                if (arg.empty()) {
                    restore_ports();
                    reply_magic_error("%time expects an expression");
                    return;
                }
                const auto start = std::chrono::steady_clock::now();
                auto display_value = driver_->eval_to_display(arg);
                const auto end = std::chrono::steady_clock::now();
                restore_ports();
                if (display_value.tag == eta::session::DisplayTag::Error) {
                    reply_error_from_driver();
                    return;
                }

                const auto wall_ms = std::chrono::duration<double, std::milli>(end - start).count();
                if (!config.silent) {
                    std::ostringstream timing;
                    timing.setf(std::ios::fixed);
                    timing.precision(3);
                    timing << "[time] wall_ms=" << wall_ms << " gc_ms=0.000\n";
                    publish_stream("stdout", timing.str());

                    auto rendered = eta::jupyter::display::render_display_value(
                        *driver_, display_value, render_options_);
                    if (!rendered.data.empty()) {
                        publish_execution_result(execution_counter,
                                                 std::move(rendered.data),
                                                 std::move(rendered.metadata));
                    }
                }
                cb(xeus::create_successful_reply());
                return;
            }
            case eta::jupyter::MagicName::Timeit: {
                if (arg.empty()) {
                    restore_ports();
                    reply_magic_error("%timeit expects an expression");
                    return;
                }

                constexpr int kMinIterations = 10;
                constexpr int kMaxIterations = 1000;
                constexpr auto kMinDuration = std::chrono::seconds(1);

                std::vector<double> samples_ms;
                samples_ms.reserve(32);
                const auto suite_start = std::chrono::steady_clock::now();

                int iters = 0;
                while ((iters < kMinIterations ||
                        (std::chrono::steady_clock::now() - suite_start) < kMinDuration) &&
                       iters < kMaxIterations) {
                    const auto t0 = std::chrono::steady_clock::now();
                    auto display_value = driver_->eval_to_display(arg);
                    const auto t1 = std::chrono::steady_clock::now();
                    if (display_value.tag == eta::session::DisplayTag::Error) {
                        restore_ports();
                        reply_error_from_driver();
                        return;
                    }
                    samples_ms.push_back(
                        std::chrono::duration<double, std::milli>(t1 - t0).count());
                    ++iters;
                }

                restore_ports();
                const double mean = std::accumulate(
                    samples_ms.begin(), samples_ms.end(), 0.0) /
                    static_cast<double>(samples_ms.size());
                double variance = 0.0;
                for (const double sample : samples_ms) {
                    const double d = sample - mean;
                    variance += d * d;
                }
                variance /= static_cast<double>(samples_ms.size());
                const double stddev = std::sqrt(variance);

                if (!config.silent) {
                    std::ostringstream timing;
                    timing.setf(std::ios::fixed);
                    timing.precision(3);
                    timing << "[timeit] n=" << samples_ms.size()
                           << " mean_ms=" << mean
                           << " std_ms=" << stddev << '\n';
                    publish_stream("stdout", timing.str());
                }
                cb(xeus::create_successful_reply());
                return;
            }
            case eta::jupyter::MagicName::Bytecode: {
                if (arg.empty()) {
                    restore_ports();
                    reply_magic_error("%bytecode expects an expression");
                    return;
                }
                std::string disasm_error;
                auto disasm_text = disassemble_source(*driver_, arg, &disasm_error);
                restore_ports();
                if (!disasm_text) {
                    reply_magic_error(disasm_error.empty() ? "bytecode generation failed"
                                                           : disasm_error);
                    return;
                }

                if (!config.silent) {
                    nl::json data = nl::json::object();
                    data["text/plain"] = *disasm_text;
                    data["text/html"] = "<pre>" + html_escape(*disasm_text) + "</pre>";
                    publish_execution_result(execution_counter, std::move(data), nl::json::object());
                }
                cb(xeus::create_successful_reply());
                return;
            }
            case eta::jupyter::MagicName::Load: {
                if (arg.empty()) {
                    restore_ports();
                    reply_magic_error("%load expects a path");
                    return;
                }
                if ((arg.front() == '"' && arg.back() == '"' && arg.size() >= 2) ||
                    (arg.front() == '\'' && arg.back() == '\'' && arg.size() >= 2)) {
                    arg = arg.substr(1, arg.size() - 2);
                }

                std::ifstream in(arg, std::ios::in | std::ios::binary);
                if (!in) {
                    restore_ports();
                    reply_magic_error("cannot open file: " + arg);
                    return;
                }
                std::ostringstream content;
                content << in.rdbuf();
                restore_ports();
                nl::json payload = nl::json::array({
                    nl::json::object({
                        {"source", "set_next_input"},
                        {"text", content.str()},
                        {"replace", true},
                    }),
                });
                cb(xeus::create_successful_reply(payload));
                return;
            }
            case eta::jupyter::MagicName::Run: {
                if (arg.empty()) {
                    restore_ports();
                    reply_magic_error("%run expects a path");
                    return;
                }
                if ((arg.front() == '"' && arg.back() == '"' && arg.size() >= 2) ||
                    (arg.front() == '\'' && arg.back() == '\'' && arg.size() >= 2)) {
                    arg = arg.substr(1, arg.size() - 2);
                }
                code_to_eval = "(load " + eta_string_literal(arg) + ")";
                break;
            }
            case eta::jupyter::MagicName::Env: {
                restore_ports();
                if (arg.empty()) {
                    publish_stream("stdout", "%env KEY or %env KEY=VALUE\n");
                    cb(xeus::create_successful_reply());
                    return;
                }
                if (auto assignment = split_env_assignment(arg)) {
#if defined(_WIN32)
                    _putenv_s(assignment->first.c_str(), assignment->second.c_str());
#else
                    setenv(assignment->first.c_str(), assignment->second.c_str(), 1);
#endif
                    publish_stream("stdout", assignment->first + "=" + assignment->second + "\n");
                    cb(xeus::create_successful_reply());
                    return;
                }
                if (const char* value = std::getenv(arg.c_str())) {
                    publish_stream("stdout", arg + "=" + std::string(value) + "\n");
                } else {
                    publish_stream("stdout", arg + " is unset\n");
                }
                cb(xeus::create_successful_reply());
                return;
            }
            case eta::jupyter::MagicName::Cwd: {
                namespace fs = std::filesystem;
                if (!arg.empty()) {
                    try {
                        fs::current_path(fs::path(arg));
                    } catch (const std::exception& e) {
                        restore_ports();
                        reply_magic_error(e.what());
                        return;
                    }
                }
                const auto cwd = fs::current_path().string();
                restore_ports();
                if (!config.silent) {
                    publish_stream("stdout", cwd + "\n");
                }
                cb(xeus::create_successful_reply());
                return;
            }
            case eta::jupyter::MagicName::Import: {
                if (arg.empty()) {
                    restore_ports();
                    reply_magic_error("%import expects a module name");
                    return;
                }
                code_to_eval = "(import " + arg + ")";
                break;
            }
            case eta::jupyter::MagicName::Reload: {
                if (arg.empty()) {
                    restore_ports();
                    reply_magic_error("%reload expects a module name");
                    return;
                }
                driver_->clear_module_cache(arg);
                code_to_eval = "(import " + arg + ")";
                break;
            }
            case eta::jupyter::MagicName::Who: {
                std::vector<std::string> names;
                names.reserve(driver_->global_names().size());
                for (const auto& [_, full] : driver_->global_names()) {
                    names.push_back(full);
                }
                std::sort(names.begin(), names.end());
                std::ostringstream oss;
                for (const auto& name : names) {
                    oss << name << '\n';
                }
                restore_ports();
                if (!config.silent) {
                    publish_stream("stdout", oss.str());
                }
                cb(xeus::create_successful_reply());
                return;
            }
            case eta::jupyter::MagicName::Whos: {
                std::vector<std::pair<std::string, std::string>> rows;
                rows.reserve(driver_->global_names().size());
                const auto& globals = driver_->vm().globals();
                for (const auto& [slot, full] : driver_->global_names()) {
                    std::string type = "Unknown";
                    if (slot < globals.size()) {
                        type = value_type_name(*driver_, globals[slot]);
                    }
                    rows.emplace_back(full, std::move(type));
                }
                std::sort(rows.begin(), rows.end(),
                          [](const auto& lhs, const auto& rhs) {
                              return lhs.first < rhs.first;
                          });
                std::ostringstream oss;
                for (const auto& [name, type] : rows) {
                    oss << name << '\t' << type << '\n';
                }
                restore_ports();
                if (!config.silent) {
                    publish_stream("stdout", oss.str());
                }
                cb(xeus::create_successful_reply());
                return;
            }
            case eta::jupyter::MagicName::Plot: {
                if (arg.empty()) {
                    restore_ports();
                    reply_magic_error("%plot expects an expression");
                    return;
                }
                code_to_eval = "(begin (import std.jupyter) (jupyter:vega " + arg + "))";
                break;
            }
            case eta::jupyter::MagicName::Table: {
                if (arg.empty()) {
                    restore_ports();
                    reply_magic_error("%table expects an expression");
                    return;
                }
                code_to_eval = "(begin (import std.jupyter) (jupyter:table " + arg + "))";
                break;
            }
            case eta::jupyter::MagicName::Trace: {
                if (magic.kind != eta::jupyter::MagicKind::Cell) {
                    restore_ports();
                    reply_magic_error("%%trace must be used as a cell magic");
                    return;
                }
                const auto body = trim_copy(magic.body);
                if (body.empty()) {
                    restore_ports();
                    reply_magic_error("%%trace cell body is empty");
                    return;
                }
                code_to_eval = body;
                trace_source = body;
                emit_trace_widget = true;
                break;
            }
            case eta::jupyter::MagicName::Unknown:
            default:
                restore_ports();
                reply_magic_error("unknown magic: %" + magic.name_text);
                return;
        }
    }

    const auto display_value = driver_->eval_to_display(code_to_eval);
    restore_ports();
    if (display_value.tag == eta::session::DisplayTag::Error) {
        reply_error_from_driver();
        return;
    }

    if (!config.silent) {
        auto rendered = eta::jupyter::display::render_display_value(
            *driver_, display_value, render_options_);
        if (!rendered.data.empty()) {
            publish_execution_result(execution_counter,
                                     std::move(rendered.data),
                                     std::move(rendered.metadata));
        }
    }

    if (!config.silent && emit_trace_widget) {
        std::string disasm_error;
        auto disasm = disassemble_source(*driver_, trace_source, &disasm_error);
        if (disasm) {
            nl::json data = nl::json::object();
            data["text/html"] =
                "<details><summary>VM Trace</summary><pre>"
                + html_escape(*disasm)
                + "</pre></details>";
            data["text/plain"] = *disasm;
            display_data(std::move(data), nl::json::object(), nl::json::object());
        } else if (!disasm_error.empty()) {
            publish_stream("stderr", "[trace] " + disasm_error + "\n");
        }
    }

    cb(xeus::create_successful_reply());
}

nl::json EtaInterpreter::complete_request_impl(const std::string& code,
                                               int cursor_pos) {
    std::lock_guard<std::mutex> lk(driver_mu_);
    if (!driver_) {
        return xeus::create_complete_reply(nl::json::array(), 0, 0);
    }

    const std::size_t clamped = static_cast<std::size_t>((std::max)(cursor_pos, 0));
    auto completion = driver_->completions_at(code, clamped);

    nl::json matches = nl::json::array();
    for (const auto& m : completion.matches) {
        matches.push_back(m);
    }

    return xeus::create_complete_reply(
        std::move(matches),
        static_cast<int>(completion.cursor_start),
        static_cast<int>(completion.cursor_end));
}

nl::json EtaInterpreter::inspect_request_impl(const std::string& code,
                                              int cursor_pos,
                                              int /*detail_level*/) {
    std::lock_guard<std::mutex> lk(driver_mu_);
    if (!driver_) return xeus::create_inspect_reply();

    const std::size_t clamped = static_cast<std::size_t>((std::max)(cursor_pos, 0));
    const auto tok = eta::interpreter::repl_complete::token_at(code, clamped);
    if (tok.text.empty()) return xeus::create_inspect_reply();

    auto md = driver_->hover_at(tok.text);
    if (md.empty()) return xeus::create_inspect_reply();

    nl::json data;
    data["text/markdown"] = std::move(md);
    return xeus::create_inspect_reply(true, std::move(data), nl::json::object());
}

nl::json EtaInterpreter::is_complete_request_impl(const std::string& code) {
    std::lock_guard<std::mutex> lk(driver_mu_);
    if (!driver_) return xeus::create_is_complete_reply("unknown");

    std::string indent;
    const bool complete = driver_->is_complete_expression(code, &indent);
    return xeus::create_is_complete_reply(complete ? "complete" : "incomplete",
                                          complete ? "" : indent);
}

nl::json EtaInterpreter::kernel_info_request_impl() {
    return xeus::create_info_reply(
        "eta",               // implementation
        "0.1.0",             // implementation_version
        "eta",               // language_name
        "0.1.0",             // language_version
        "text/x-eta",        // language_mimetype
        ".eta",              // language_file_extension
        "scheme",            // pygments_lexer
        std::string("scheme"), // codemirror_mode
        "",                  // nbconvert_exporter
        "Eta Jupyter kernel" // banner
    );
}

nl::json EtaInterpreter::shutdown_request_impl(bool restart) {
    return xeus::create_shutdown_reply(restart);
}

nl::json EtaInterpreter::interrupt_request_impl() {
    request_interrupt();
    return xeus::create_interrupt_reply();
}

void EtaInterpreter::register_comm_targets() {
    if (comm_targets_registered_) return;
    comm_targets_registered_ = true;

    register_comm_target(std::string(eta::jupyter::comm::heap_target()));
    register_comm_target(std::string(eta::jupyter::comm::disasm_target()));
    register_comm_target(std::string(eta::jupyter::comm::actors_target()));
    register_comm_target(std::string(eta::jupyter::comm::dag_target()));
    register_comm_target(std::string(eta::jupyter::comm::tensor_target()));

    if (driver_) {
        driver_->on_actor_lifecycle([this](const eta::session::Driver::ActorEvent& event) {
            handle_actor_lifecycle_event(event);
        });
    }
}

void EtaInterpreter::register_comm_target(const std::string& target_name) {
    comm_manager().register_comm_target(
        target_name,
        [this, target_name](xeus::xcomm&& comm, xeus::xmessage request) {
            handle_comm_open(target_name, std::move(comm), std::move(request));
        });
}

void EtaInterpreter::handle_comm_open(const std::string& target_name,
                                      xeus::xcomm&& comm,
                                      xeus::xmessage request) {
    const std::string comm_id = std::string(comm.id());
    comm.on_message([this, target_name, comm_id](xeus::xmessage msg) {
        handle_comm_message(target_name, comm_id, std::move(msg));
    });
    comm.on_close([this, target_name, comm_id](xeus::xmessage /*msg*/) {
        remove_comm(target_name, comm_id);
    });

    {
        std::lock_guard<std::mutex> lk(comm_mu_);
        comms_[target_name][comm_id] = std::make_unique<xeus::xcomm>(std::move(comm));
    }

    nl::json request_data = nl::json::object();
    if (request.content().contains("data")) {
        request_data = request.content()["data"];
    }

    nl::json payload = nl::json::object({
        {"event", "snapshot"},
        {"target", target_name},
        {"payload", build_comm_snapshot(target_name, request_data)},
    });

    std::lock_guard<std::mutex> lk(comm_mu_);
    auto by_target = comms_.find(target_name);
    if (by_target == comms_.end()) return;
    auto it = by_target->second.find(comm_id);
    if (it == by_target->second.end() || !it->second) return;
    try {
        it->second->send(nl::json::object(), payload, xeus::buffer_sequence{});
    } catch (...) {
    }
}

void EtaInterpreter::handle_comm_message(const std::string& target_name,
                                         const std::string& /*comm_id*/,
                                         xeus::xmessage request) {
    nl::json request_data = nl::json::object();
    if (request.content().contains("data")) {
        request_data = request.content()["data"];
    }

    const std::string req = request_data.value("request", std::string("snapshot"));
    if (req == "close") return;
    if (req == "ping") {
        broadcast_comm(target_name, nl::json::object({
            {"event", "pong"},
            {"target", target_name},
        }));
        return;
    }

    nl::json payload = nl::json::object({
        {"event", "snapshot"},
        {"target", target_name},
        {"payload", build_comm_snapshot(target_name, request_data)},
    });
    broadcast_comm(target_name, payload);
}

void EtaInterpreter::remove_comm(const std::string& target_name,
                                 const std::string& comm_id) {
    std::lock_guard<std::mutex> lk(comm_mu_);
    auto by_target = comms_.find(target_name);
    if (by_target == comms_.end()) return;
    by_target->second.erase(comm_id);
    if (by_target->second.empty()) {
        comms_.erase(by_target);
    }
}

void EtaInterpreter::broadcast_comm(const std::string& target_name,
                                    const nl::json& payload) {
    std::lock_guard<std::mutex> lk(comm_mu_);
    auto by_target = comms_.find(target_name);
    if (by_target == comms_.end()) return;
    for (auto& [_, comm_ptr] : by_target->second) {
        if (!comm_ptr) continue;
        try {
            comm_ptr->send(nl::json::object(), payload, xeus::buffer_sequence{});
        } catch (...) {
        }
    }
}

nl::json EtaInterpreter::build_comm_snapshot(const std::string& target_name,
                                             const nl::json& request_data) {
    std::lock_guard<std::mutex> lk(driver_mu_);
    if (!driver_) return nl::json::object();

    if (target_name == eta::jupyter::comm::heap_target()) {
        return eta::jupyter::comm::build_heap_snapshot(*driver_);
    }

    if (target_name == eta::jupyter::comm::disasm_target()) {
        const std::string scope = request_data.value("scope", std::string("current"));
        const std::string function = request_data.value("function", std::string());
        return eta::jupyter::comm::build_disassembly(*driver_, scope, function);
    }

    if (target_name == eta::jupyter::comm::actors_target()) {
        return eta::jupyter::comm::build_actor_snapshot(*driver_);
    }

    if (target_name == eta::jupyter::comm::dag_target()) {
        nl::json graph = request_data.contains("graph")
            ? request_data["graph"]
            : nl::json::object();
        return eta::jupyter::comm::build_dag_payload(std::move(graph));
    }

    if (target_name == eta::jupyter::comm::tensor_target()) {
        nl::json tensor = request_data.contains("tensor")
            ? request_data["tensor"]
            : nl::json::object();
        return eta::jupyter::comm::build_tensor_payload(std::move(tensor));
    }

    return nl::json::object();
}

void EtaInterpreter::handle_actor_lifecycle_event(const eta::session::Driver::ActorEvent& event) {
    const std::string kind = (event.kind == eta::session::Driver::ActorEvent::Kind::Started)
        ? "started"
        : "exited";
    const auto event_payload = eta::jupyter::comm::build_actor_event(kind, event.index, event.name);

    broadcast_comm(
        std::string(eta::jupyter::comm::actors_target()),
        nl::json::object({
            {"event", "lifecycle"},
            {"target", std::string(eta::jupyter::comm::actors_target())},
            {"payload", event_payload},
        }));
}

void EtaInterpreter::apply_startup_configuration() {
    if (configured_) return;
    configured_ = true;
    set_kernel_environment();

    if (!driver_) return;

    std::lock_guard<std::mutex> lk(driver_mu_);
    auto is_safe_module_name = [](const std::string& module) {
        if (module.empty()) return false;
        return std::all_of(module.begin(), module.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == ':';
        });
    };

    for (const auto& module : kernel_config_.autoimport_modules) {
        if (!is_safe_module_name(module)) {
            std::cerr << "[eta-jupyter] skipping invalid auto-import module: "
                      << module << '\n';
            continue;
        }

        const std::string import_form = "(import " + module + ")";
        if (!driver_->run_source(import_form)) {
            std::cerr << "[eta-jupyter] auto-import failed for " << module << ":\n"
                      << diagnostics_to_text() << '\n';
        }
    }
}

void EtaInterpreter::set_kernel_environment() {
#if defined(_WIN32)
    _putenv_s("ETA_KERNEL", "1");
#else
    setenv("ETA_KERNEL", "1", 1);
#endif
}

std::string EtaInterpreter::diagnostics_to_text() const {
    if (!driver_) return {};
    std::ostringstream oss;
    driver_->diagnostics().print_all(oss, /*use_color=*/false, driver_->file_resolver());
    return oss.str();
}

std::vector<std::string> EtaInterpreter::traceback_lines(const std::string& ename,
                                                         const std::string& evalue) const {
    std::vector<std::string> lines;
    lines.push_back(ename + ": " + evalue);

    if (!driver_) return lines;
    for (const auto& d : driver_->diagnostics().diagnostics()) {
        std::ostringstream oss;
        eta::diagnostic::format_diagnostic(
            oss, d, /*use_color=*/false, driver_->file_resolver());
        auto line = oss.str();
        if (!line.empty()) lines.push_back(std::move(line));
    }

    const auto frames = driver_->vm().get_frames();
    for (const auto& f : frames) {
        const auto* file = driver_->path_for_file_id(f.span.file_id);
        std::ostringstream oss;
        oss << "  at ";
        if (file) {
            oss << file->filename().string();
        } else {
            oss << "file " << f.span.file_id;
        }
        oss << ":" << f.span.start.line << ":" << f.span.start.column
            << " in " << f.func_name;
        lines.push_back(oss.str());
    }

    return lines;
}

} ///< namespace eta::jupyter
