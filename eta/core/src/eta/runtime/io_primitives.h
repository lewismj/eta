#pragma once

#include <expected>
#include <functional>
#include <string>
#include <vector>

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/value_formatter.h"
#include "eta/runtime/vm/vm.h"

namespace eta::runtime {

using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::error;

/**
 * @brief Register I/O primitives that require VM access.
 *
 * This must be called AFTER the VM is created, unlike core primitives.
 *
 * Primitives registered:
 *   - display (port-aware version)
 *   - write   (port-aware version, machine-readable)
 *   - newline (port-aware version)
 */
inline void register_io_primitives(BuiltinEnvironment& env, Heap& heap, InternTable& intern_table, vm::VM& vm) {
    using Args = const std::vector<LispVal>&;

    /// Helper to extract port from a LispVal
    auto get_port = [&heap](LispVal val) -> std::expected<types::PortObject*, RuntimeError> {
        if (!ops::is_boxed(val) || ops::tag(val) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "not a port"}});
        }
        auto* port_obj = heap.try_get_as<ObjectKind::Port, types::PortObject>(ops::payload(val));
        if (!port_obj) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "not a port"}});
        }
        return port_obj;
    };

    /// Helper to write a formatted value to a port (or stdout fallback)
    auto write_to_port = [&vm, get_port](const std::string& output, const std::vector<LispVal>& args) -> std::expected<LispVal, RuntimeError> {
        LispVal port_val = args.size() > 1 ? args[1] : vm.current_output_port();

        auto port_obj = get_port(port_val);
        if (!port_obj) {
            /// Fallback to stdout if port is invalid
            std::cout << output;
            return nanbox::Nil;
        }

        auto result = (*port_obj)->port->write_string(output);
        if (!result) return std::unexpected(result.error());
        return nanbox::Nil;
    };

    /**
     * Port-aware I/O: display write newline
     */

    env.register_builtin("display", 1, true, [&heap, &intern_table, write_to_port](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "display: requires at least 1 argument"}});
        }
        std::string output = format_value(args[0], FormatMode::Display, heap, intern_table);
        return write_to_port(output, args);
    });

    env.register_builtin("write", 1, true, [&heap, &intern_table, write_to_port](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "write: requires at least 1 argument"}});
        }
        std::string output = format_value(args[0], FormatMode::Write, heap, intern_table);
        return write_to_port(output, args);
    });

    env.register_builtin("newline", 0, true, [&vm, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        /// Optional port argument (defaults to current-output-port)
        LispVal port_val = args.empty() ? vm.current_output_port() : args[0];

        auto port_obj = get_port(port_val);
        if (!port_obj) {
            /// Fallback to stdout if port is invalid
            std::cout << '\n';
            return nanbox::Nil;
        }

        auto result = (*port_obj)->port->write_string("\n");
        if (!result) return std::unexpected(result.error());

        return nanbox::Nil;
    });
}

}  ///< namespace eta::runtime

