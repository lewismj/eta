#pragma once

#include <cstddef>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>

#include "eta/log/log_payload.h"
#include "eta/log/log_port_sink.h"
#include "eta/log/log_state.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/error.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/vm/vm.h"

namespace eta::log {

using eta::runtime::BuiltinEnvironment;
using eta::runtime::error::RuntimeError;
using eta::runtime::error::RuntimeErrorCode;
using eta::runtime::error::VMError;
using eta::runtime::memory::factory::make_log_logger;
using eta::runtime::memory::factory::make_log_sink;
using eta::runtime::memory::factory::make_symbol;
using eta::runtime::memory::heap::Heap;
using eta::runtime::memory::heap::ObjectKind;
using eta::runtime::memory::intern::InternTable;
using eta::runtime::nanbox::False;
using eta::runtime::nanbox::LispVal;
using eta::runtime::nanbox::Nil;
using eta::runtime::nanbox::Tag;
namespace ops = eta::runtime::nanbox::ops;
using eta::runtime::types::LogFormatterMode;
using Args = std::span<const LispVal>;

namespace {

inline constexpr const char* kHumanPattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v";
inline constexpr const char* kJsonPattern = "%v";

inline std::unexpected<RuntimeError> type_error(const std::string& msg) {
    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, msg}});
}

inline std::unexpected<RuntimeError> internal_error(const std::string& msg) {
    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InternalError, msg}});
}

inline std::expected<std::string, RuntimeError> require_string(
    InternTable& intern, LispVal value, const char* who, const char* label) {
    auto sv = eta::runtime::StringView::try_from(value, intern);
    if (!sv) return type_error(std::string(who) + ": " + label + " must be a string");
    return std::string(sv->view());
}

inline std::expected<std::string, RuntimeError> require_symbol_name(
    InternTable& intern, LispVal value, const char* who, const char* label) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::Symbol) {
        return type_error(std::string(who) + ": " + label + " must be a symbol");
    }
    auto text = intern.get_string(ops::payload(value));
    if (!text) return internal_error(std::string(who) + ": unresolved symbol id");
    return std::string(*text);
}

inline std::expected<bool, RuntimeError> require_bool(
    LispVal value, const char* who, const char* label) {
    if (value == eta::runtime::nanbox::True) return true;
    if (value == eta::runtime::nanbox::False) return false;
    return type_error(std::string(who) + ": " + label + " must be #t or #f");
}

inline std::expected<std::size_t, RuntimeError> require_size(
    Heap& heap, LispVal value, const char* who, const char* label) {
    auto n = eta::runtime::classify_numeric(value, heap);
    if (!n.is_valid() || n.is_flonum() || n.int_val < 0) {
        return type_error(std::string(who) + ": " + label + " must be a non-negative integer");
    }
    return static_cast<std::size_t>(n.int_val);
}

inline std::expected<eta::runtime::types::PortObject*, RuntimeError> require_output_port(
    Heap& heap, LispVal value, const char* who) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
        return type_error(std::string(who) + ": expected an output port");
    }
    auto* port_obj = heap.try_get_as<ObjectKind::Port, eta::runtime::types::PortObject>(ops::payload(value));
    if (!port_obj || !port_obj->port || !port_obj->port->is_output()) {
        return type_error(std::string(who) + ": expected an output port");
    }
    return port_obj;
}

inline std::expected<eta::runtime::types::LogSink*, RuntimeError> require_log_sink(
    Heap& heap, LispVal value, const char* who, const char* label = "sink") {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
        return type_error(std::string(who) + ": " + label + " must be a log sink");
    }
    auto* sink = heap.try_get_as<ObjectKind::LogSink, eta::runtime::types::LogSink>(ops::payload(value));
    if (!sink) return type_error(std::string(who) + ": " + label + " must be a log sink");
    return sink;
}

inline std::expected<eta::runtime::types::LogLogger*, RuntimeError> require_log_logger(
    Heap& heap, LispVal value, const char* who, const char* label = "logger") {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
        return type_error(std::string(who) + ": " + label + " must be a logger");
    }
    auto* logger = heap.try_get_as<ObjectKind::LogLogger, eta::runtime::types::LogLogger>(ops::payload(value));
    if (!logger) return type_error(std::string(who) + ": " + label + " must be a logger");
    return logger;
}

inline std::expected<std::shared_ptr<SinkState>, RuntimeError> sink_state_from_obj(
    eta::runtime::types::LogSink* sink, const char* who) {
    auto state = std::static_pointer_cast<SinkState>(sink->state);
    if (!state || !state->sink) {
        return internal_error(std::string(who) + ": invalid log sink state");
    }
    return state;
}

inline std::expected<std::shared_ptr<LoggerState>, RuntimeError> logger_state_from_obj(
    eta::runtime::types::LogLogger* logger, const char* who) {
    auto state = std::static_pointer_cast<LoggerState>(logger->state);
    if (!state || !state->logger) {
        return internal_error(std::string(who) + ": invalid logger state");
    }
    return state;
}

inline std::expected<LispVal, RuntimeError> wrap_sink(Heap& heap, const std::shared_ptr<SinkState>& state) {
    return make_log_sink(heap, eta::runtime::types::LogSink{
        .state = std::static_pointer_cast<void>(state),
        .is_port_sink = state->is_port_sink,
        .is_current_error_sink = state->is_current_error_sink
    });
}

inline std::expected<LispVal, RuntimeError> wrap_logger(Heap& heap, const std::shared_ptr<LoggerState>& state) {
    return make_log_logger(heap, eta::runtime::types::LogLogger{
        .state = std::static_pointer_cast<void>(state),
        .name = state->name,
        .formatter_mode = state->formatter_mode
    });
}

inline const char* level_name(spdlog::level::level_enum level) {
    switch (level) {
        case spdlog::level::trace: return "trace";
        case spdlog::level::debug: return "debug";
        case spdlog::level::info: return "info";
        case spdlog::level::warn: return "warn";
        case spdlog::level::err: return "error";
        case spdlog::level::critical: return "critical";
        case spdlog::level::off: return "off";
        default: return "info";
    }
}

inline std::optional<spdlog::level::level_enum> parse_level_name(std::string_view text) {
    if (text == "trace") return spdlog::level::trace;
    if (text == "debug") return spdlog::level::debug;
    if (text == "info") return spdlog::level::info;
    if (text == "warn" || text == "warning") return spdlog::level::warn;
    if (text == "error" || text == "err") return spdlog::level::err;
    if (text == "critical") return spdlog::level::critical;
    if (text == "off") return spdlog::level::off;
    return std::nullopt;
}

inline std::string normalized_level_name(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

inline std::expected<spdlog::level::level_enum, RuntimeError> parse_level(
    InternTable& intern, LispVal value, const char* who, const char* label) {
    auto sym = require_symbol_name(intern, value, who, label);
    if (!sym) return std::unexpected(sym.error());
    auto parsed = parse_level_name(*sym);
    if (parsed) return *parsed;

    return type_error(std::string(who) + ": unknown level symbol '" + *sym + "'");
}

inline std::expected<LispVal, RuntimeError> level_to_symbol(InternTable& intern, spdlog::level::level_enum level) {
    auto sym = make_symbol(intern, level_name(level));
    if (!sym) return std::unexpected(sym.error());
    return *sym;
}

inline std::expected<LogFormatterMode, RuntimeError> parse_formatter_mode(
    InternTable& intern, LispVal value, const char* who) {
    auto sym = require_symbol_name(intern, value, who, "formatter");
    if (!sym) return std::unexpected(sym.error());
    if (*sym == "human") return LogFormatterMode::Human;
    if (*sym == "json") return LogFormatterMode::Json;
    return type_error(std::string(who) + ": formatter must be 'human or 'json");
}

struct SinkBundle {
    std::vector<spdlog::sink_ptr> sinks;
    bool has_port_sink{false};
};

inline std::expected<SinkBundle, RuntimeError> collect_sinks(Heap& heap, LispVal list_val, const char* who) {
    SinkBundle bundle;
    LispVal cur = list_val;
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
            return type_error(std::string(who) + ": sinks must be a proper list of log sinks");
        }
        auto* cell = heap.try_get_as<ObjectKind::Cons, eta::runtime::types::Cons>(ops::payload(cur));
        if (!cell) {
            return type_error(std::string(who) + ": sinks must be a proper list of log sinks");
        }

        auto sink_obj = require_log_sink(heap, cell->car, who);
        if (!sink_obj) return std::unexpected(sink_obj.error());
        auto sink_state = sink_state_from_obj(*sink_obj, who);
        if (!sink_state) return std::unexpected(sink_state.error());

        bundle.sinks.push_back((*sink_state)->sink);
        bundle.has_port_sink = bundle.has_port_sink || (*sink_state)->is_port_sink;
        cur = cell->cdr;
    }

    if (bundle.sinks.empty()) {
        return type_error(std::string(who) + ": at least one sink is required");
    }
    return bundle;
}

inline std::shared_ptr<LoggerState> create_logger_state(
    const std::string& name,
    const std::vector<spdlog::sink_ptr>& sinks,
    bool has_port_sink) {
    auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::err);
    logger->set_pattern(kHumanPattern);

    auto state = std::make_shared<LoggerState>();
    state->logger = std::move(logger);
    state->name = name;
    state->formatter_mode = LogFormatterMode::Human;
    state->has_port_sink = has_port_sink;
    return state;
}

inline void apply_env_log_level(const std::shared_ptr<LoggerState>& state) {
    if (!state || !state->logger) return;
    const char* env = std::getenv("ETA_LOG_LEVEL");
    if (!env || env[0] == '\0') return;

    auto parsed = parse_level_name(normalized_level_name(env));
    if (parsed) {
        state->logger->set_level(*parsed);
    }
}

inline void register_named_logger_if_needed(const std::shared_ptr<LoggerState>& state) {
    if (!state || state->name.empty()) return;
    spdlog::drop(state->name);
    spdlog::register_logger(state->logger);
    global_log_state().remember_named_logger(state->name, state);
}

} // namespace

inline void register_log_primitives(
    BuiltinEnvironment& env,
    Heap& heap,
    InternTable& intern,
    eta::runtime::vm::VM* vm = nullptr) {

    env.register_builtin("%log-make-stdout-sink", 2, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto color = require_bool(args[0], "%log-make-stdout-sink", "first argument");
            if (!color) return std::unexpected(color.error());

            auto stream = require_symbol_name(intern, args[1], "%log-make-stdout-sink", "second argument");
            if (!stream) return std::unexpected(stream.error());

            try {
                auto state = std::make_shared<SinkState>();
                if (*stream == "stdout") {
                    if (*color) {
                        state->sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                    } else {
                        state->sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
                    }
                } else if (*stream == "stderr") {
                    if (*color) {
                        state->sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
                    } else {
                        state->sink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
                    }
                } else {
                    return type_error("%log-make-stdout-sink: stream must be 'stdout or 'stderr");
                }
                return wrap_sink(heap, state);
            } catch (const std::exception& ex) {
                return internal_error(std::string("%log-make-stdout-sink: ") + ex.what());
            }
        });

    env.register_builtin("%log-make-file-sink", 2, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto path = require_string(intern, args[0], "%log-make-file-sink", "first argument");
            if (!path) return std::unexpected(path.error());
            auto truncate = require_bool(args[1], "%log-make-file-sink", "second argument");
            if (!truncate) return std::unexpected(truncate.error());

            try {
                auto normalized = std::filesystem::path(*path).lexically_normal().string();
                auto state = std::make_shared<SinkState>();
                state->sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(normalized, *truncate);
                return wrap_sink(heap, state);
            } catch (const std::exception& ex) {
                return internal_error(std::string("%log-make-file-sink: ") + ex.what());
            }
        });

    env.register_builtin("%log-make-rotating-sink", 3, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto path = require_string(intern, args[0], "%log-make-rotating-sink", "first argument");
            if (!path) return std::unexpected(path.error());
            auto max_size = require_size(heap, args[1], "%log-make-rotating-sink", "second argument");
            if (!max_size) return std::unexpected(max_size.error());
            auto max_files = require_size(heap, args[2], "%log-make-rotating-sink", "third argument");
            if (!max_files) return std::unexpected(max_files.error());
            if (*max_size == 0 || *max_files == 0) {
                return type_error("%log-make-rotating-sink: max-size and max-files must be > 0");
            }

            try {
                auto normalized = std::filesystem::path(*path).lexically_normal().string();
                auto state = std::make_shared<SinkState>();
                state->sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    normalized, *max_size, *max_files);
                return wrap_sink(heap, state);
            } catch (const std::exception& ex) {
                return internal_error(std::string("%log-make-rotating-sink: ") + ex.what());
            }
        });

    env.register_builtin("%log-make-daily-sink", 4, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto path = require_string(intern, args[0], "%log-make-daily-sink", "first argument");
            if (!path) return std::unexpected(path.error());
            auto hour = require_size(heap, args[1], "%log-make-daily-sink", "second argument");
            if (!hour) return std::unexpected(hour.error());
            auto minute = require_size(heap, args[2], "%log-make-daily-sink", "third argument");
            if (!minute) return std::unexpected(minute.error());
            auto max_files = require_size(heap, args[3], "%log-make-daily-sink", "fourth argument");
            if (!max_files) return std::unexpected(max_files.error());
            if (*hour > 23 || *minute > 59) {
                return type_error("%log-make-daily-sink: hour/minute out of range");
            }

            try {
                auto normalized = std::filesystem::path(*path).lexically_normal().string();
                auto state = std::make_shared<SinkState>();
                state->sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
                    normalized,
                    static_cast<int>(*hour),
                    static_cast<int>(*minute),
                    false,
                    static_cast<std::uint16_t>(*max_files));
                return wrap_sink(heap, state);
            } catch (const std::exception& ex) {
                return internal_error(std::string("%log-make-daily-sink: ") + ex.what());
            }
        });

    env.register_builtin("%log-make-port-sink", 1, false,
        [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            auto port_obj = require_output_port(heap, args[0], "%log-make-port-sink");
            if (!port_obj) return std::unexpected(port_obj.error());
            (void)port_obj;

            auto state = std::make_shared<SinkState>();
            state->sink = std::make_shared<EtaPortSink>(heap, vm, args[0], false);
            state->is_port_sink = true;
            return wrap_sink(heap, state);
        });

    env.register_builtin("%log-make-current-error-sink", 0, false,
        [&heap, vm](Args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return internal_error("%log-make-current-error-sink: VM is not available");
            auto state = std::make_shared<SinkState>();
            state->sink = std::make_shared<EtaPortSink>(heap, vm, Nil, true);
            state->is_port_sink = true;
            state->is_current_error_sink = true;
            return wrap_sink(heap, state);
        });

    env.register_builtin("%log-make-logger", 2, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto name = require_string(intern, args[0], "%log-make-logger", "first argument");
            if (!name) return std::unexpected(name.error());
            auto bundle = collect_sinks(heap, args[1], "%log-make-logger");
            if (!bundle) return std::unexpected(bundle.error());

            try {
                auto state = create_logger_state(*name, bundle->sinks, bundle->has_port_sink);
                register_named_logger_if_needed(state);
                return wrap_logger(heap, state);
            } catch (const std::exception& ex) {
                return internal_error(std::string("%log-make-logger: ") + ex.what());
            }
        });

    env.register_builtin("%log-get-logger", 1, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto name = require_string(intern, args[0], "%log-get-logger", "first argument");
            if (!name) return std::unexpected(name.error());

            auto state = global_log_state().find_named_logger(*name);
            if (!state) {
                auto logger = spdlog::get(*name);
                if (!logger) return False;
                state = std::make_shared<LoggerState>();
                state->logger = std::move(logger);
                state->name = *name;
                state->formatter_mode = LogFormatterMode::Human;
                global_log_state().remember_named_logger(*name, state);
            }
            return wrap_logger(heap, state);
        });

    env.register_builtin("%log-default-logger", 0, false,
        [&heap, vm](Args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return internal_error("%log-default-logger: VM is not available");
            auto state = global_log_state().default_logger_for(*vm);
            if (!state) {
                auto sink_state = std::make_shared<SinkState>();
                sink_state->sink = std::make_shared<EtaPortSink>(heap, vm, Nil, true);
                sink_state->is_port_sink = true;
                sink_state->is_current_error_sink = true;

                state = create_logger_state("default", {sink_state->sink}, true);
                apply_env_log_level(state);
                global_log_state().set_default_logger_for(*vm, state);
            }
            return wrap_logger(heap, state);
        });

    env.register_builtin("%log-set-default!", 1, false,
        [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return internal_error("%log-set-default!: VM is not available");
            auto logger_obj = require_log_logger(heap, args[0], "%log-set-default!");
            if (!logger_obj) return std::unexpected(logger_obj.error());
            auto state = logger_state_from_obj(*logger_obj, "%log-set-default!");
            if (!state) return std::unexpected(state.error());
            global_log_state().set_default_logger_for(*vm, *state);
            return Nil;
        });

    env.register_builtin("%log-set-level!", 2, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto logger_obj = require_log_logger(heap, args[0], "%log-set-level!");
            if (!logger_obj) return std::unexpected(logger_obj.error());
            auto state = logger_state_from_obj(*logger_obj, "%log-set-level!");
            if (!state) return std::unexpected(state.error());
            auto level = parse_level(intern, args[1], "%log-set-level!", "second argument");
            if (!level) return std::unexpected(level.error());
            (*state)->logger->set_level(*level);
            return Nil;
        });

    env.register_builtin("%log-level", 1, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto logger_obj = require_log_logger(heap, args[0], "%log-level");
            if (!logger_obj) return std::unexpected(logger_obj.error());
            auto state = logger_state_from_obj(*logger_obj, "%log-level");
            if (!state) return std::unexpected(state.error());
            return level_to_symbol(intern, (*state)->logger->level());
        });

    env.register_builtin("%log-set-global-level!", 1, false,
        [&intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto level = parse_level(intern, args[0], "%log-set-global-level!", "first argument");
            if (!level) return std::unexpected(level.error());
            spdlog::set_level(*level);
            return Nil;
        });

    env.register_builtin("%log-set-pattern!", 2, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto logger_obj = require_log_logger(heap, args[0], "%log-set-pattern!");
            if (!logger_obj) return std::unexpected(logger_obj.error());
            auto state = logger_state_from_obj(*logger_obj, "%log-set-pattern!");
            if (!state) return std::unexpected(state.error());
            auto pattern = require_string(intern, args[1], "%log-set-pattern!", "second argument");
            if (!pattern) return std::unexpected(pattern.error());
            try {
                (*state)->logger->set_pattern(*pattern);
                return Nil;
            } catch (const std::exception& ex) {
                return type_error(std::string("%log-set-pattern!: invalid pattern: ") + ex.what());
            }
        });

    env.register_builtin("%log-set-formatter!", 2, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto logger_obj = require_log_logger(heap, args[0], "%log-set-formatter!");
            if (!logger_obj) return std::unexpected(logger_obj.error());
            auto state = logger_state_from_obj(*logger_obj, "%log-set-formatter!");
            if (!state) return std::unexpected(state.error());
            auto mode = parse_formatter_mode(intern, args[1], "%log-set-formatter!");
            if (!mode) return std::unexpected(mode.error());

            (*state)->formatter_mode = *mode;
            (*logger_obj)->formatter_mode = *mode;
            if (*mode == LogFormatterMode::Json) {
                (*state)->logger->set_pattern(kJsonPattern);
            } else {
                (*state)->logger->set_pattern(kHumanPattern);
            }
            return Nil;
        });

    env.register_builtin("%log-flush!", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto logger_obj = require_log_logger(heap, args[0], "%log-flush!");
            if (!logger_obj) return std::unexpected(logger_obj.error());
            auto state = logger_state_from_obj(*logger_obj, "%log-flush!");
            if (!state) return std::unexpected(state.error());
            (*state)->logger->flush();
            return Nil;
        });

    env.register_builtin("%log-flush-on!", 2, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto logger_obj = require_log_logger(heap, args[0], "%log-flush-on!");
            if (!logger_obj) return std::unexpected(logger_obj.error());
            auto state = logger_state_from_obj(*logger_obj, "%log-flush-on!");
            if (!state) return std::unexpected(state.error());
            auto level = parse_level(intern, args[1], "%log-flush-on!", "second argument");
            if (!level) return std::unexpected(level.error());
            (*state)->logger->flush_on(*level);
            return Nil;
        });

    env.register_builtin("%log-emit", 4, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto logger_obj = require_log_logger(heap, args[0], "%log-emit");
            if (!logger_obj) return std::unexpected(logger_obj.error());
            auto state = logger_state_from_obj(*logger_obj, "%log-emit");
            if (!state) return std::unexpected(state.error());
            auto level = parse_level(intern, args[1], "%log-emit", "second argument");
            if (!level) return std::unexpected(level.error());
            auto msg = require_string(intern, args[2], "%log-emit", "third argument");
            if (!msg) return std::unexpected(msg.error());

            std::expected<std::string, RuntimeError> rendered = payload::render_human(*msg, args[3], heap, intern);
            if ((*state)->formatter_mode == LogFormatterMode::Json) {
                const std::string logger_name = (*state)->name.empty() ? std::string((*state)->logger->name()) : (*state)->name;
                rendered = payload::render_json(logger_name, level_name(*level), *msg, args[3], heap, intern);
            }
            if (!rendered) return std::unexpected(rendered.error());

            (*state)->logger->log(*level, *rendered);
            return Nil;
        });

    env.register_builtin("%log-shutdown!", 0, false,
        [](Args) -> std::expected<LispVal, RuntimeError> {
            spdlog::shutdown();
            global_log_state().clear();
            return Nil;
        });
}

} ///< namespace eta::log
