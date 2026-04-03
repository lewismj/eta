#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <vector>

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/port.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/vm/vm.h"

namespace eta::runtime {

using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::error;

/**
 * @brief Register port-related primitives into a BuiltinEnvironment.
 *
 * Primitives registered:
 *   - current-input-port
 *   - current-output-port
 *   - current-error-port
 *   - set-current-input-port!
 *   - set-current-output-port!
 *   - set-current-error-port!
 *   - open-output-string
 *   - get-output-string
 *   - open-input-string
 *   - write-string
 *   - read-char
 *   - port?
 *   - input-port?
 *   - output-port?
 *   - close-port
 */
inline void register_port_primitives(BuiltinEnvironment& env, Heap& heap, InternTable& intern_table, vm::VM& vm) {
    using Args = const std::vector<LispVal>&;

    // Helper to extract port from a LispVal
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

    // ========================================================================
    // Current port accessors
    // ========================================================================

    env.register_builtin("current-input-port", 0, false, [&vm](Args) -> std::expected<LispVal, RuntimeError> {
        return vm.current_input_port();
    });

    env.register_builtin("current-output-port", 0, false, [&vm](Args) -> std::expected<LispVal, RuntimeError> {
        return vm.current_output_port();
    });

    env.register_builtin("current-error-port", 0, false, [&vm](Args) -> std::expected<LispVal, RuntimeError> {
        return vm.current_error_port();
    });

    // ========================================================================
    // Current port setters
    // ========================================================================

    env.register_builtin("set-current-input-port!", 1, false, [&vm, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        auto port_obj = get_port(args[0]);
        if (!port_obj) return std::unexpected(port_obj.error());

        if (!(*port_obj)->port->is_input()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "set-current-input-port!: not an input port"}});
        }

        vm.set_current_input_port(args[0]);
        return Nil;
    });

    env.register_builtin("set-current-output-port!", 1, false, [&vm, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        auto port_obj = get_port(args[0]);
        if (!port_obj) return std::unexpected(port_obj.error());

        if (!(*port_obj)->port->is_output()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "set-current-output-port!: not an output port"}});
        }

        vm.set_current_output_port(args[0]);
        return Nil;
    });

    env.register_builtin("set-current-error-port!", 1, false, [&vm, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        auto port_obj = get_port(args[0]);
        if (!port_obj) return std::unexpected(port_obj.error());

        if (!(*port_obj)->port->is_output()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "set-current-error-port!: not an output port"}});
        }

        vm.set_current_error_port(args[0]);
        return Nil;
    });

    // ========================================================================
    // String ports
    // ========================================================================

    env.register_builtin("open-output-string", 0, false, [&heap](Args) -> std::expected<LispVal, RuntimeError> {
        auto port = std::make_shared<StringPort>(StringPort::Mode::Output);
        return make_port(heap, port);
    });

    env.register_builtin("get-output-string", 1, false, [&heap, &intern_table, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        auto port_obj = get_port(args[0]);
        if (!port_obj) return std::unexpected(port_obj.error());

        // Try to downcast to StringPort
        auto* string_port = dynamic_cast<StringPort*>((*port_obj)->port.get());
        if (!string_port) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "get-output-string: not a string port"}});
        }

        return make_string(heap, intern_table, string_port->get_string());
    });

    env.register_builtin("open-input-string", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto sv = StringView::try_from(args[0], intern_table);
        if (!sv) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "open-input-string: not a string"}});
        }

        auto port = std::make_shared<StringPort>(StringPort::Mode::Input, std::string(sv->view()));
        return make_port(heap, port);
    });

    // ========================================================================
    // Port I/O operations
    // ========================================================================

    env.register_builtin("write-string", 1, true, [&vm, &intern_table, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "write-string: requires at least 1 argument"}});
        }

        auto sv = StringView::try_from(args[0], intern_table);
        if (!sv) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "write-string: first argument must be a string"}});
        }

        // Optional port argument (defaults to current-output-port)
        LispVal port_val = args.size() > 1 ? args[1] : vm.current_output_port();

        auto port_obj = get_port(port_val);
        if (!port_obj) return std::unexpected(port_obj.error());

        auto result = (*port_obj)->port->write_string(std::string(sv->view()));
        if (!result) return std::unexpected(result.error());

        return Nil;
    });

    env.register_builtin("read-char", 0, true, [&heap, &vm, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        // Optional port argument (defaults to current-input-port)
        LispVal port_val = args.empty() ? vm.current_input_port() : args[0];

        auto port_obj = get_port(port_val);
        if (!port_obj) return std::unexpected(port_obj.error());

        auto ch = (*port_obj)->port->read_char();
        if (!ch) {
            // EOF - return a special EOF object (we'll use False for now, Scheme typically uses an eof-object)
            return False;
        }

        auto encoded = ops::encode(*ch);
        if (!encoded) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "read-char: failed to encode character"}});
        }

        return *encoded;
    });

    // ========================================================================
    // Port predicates
    // ========================================================================

    env.register_builtin("port?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) {
            return False;
        }
        auto* port_obj = heap.try_get_as<ObjectKind::Port, types::PortObject>(ops::payload(args[0]));
        return port_obj ? True : False;
    });

    env.register_builtin("input-port?", 1, false, [get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        auto port_obj = get_port(args[0]);
        if (!port_obj) return False;
        return (*port_obj)->port->is_input() ? True : False;
    });

    env.register_builtin("output-port?", 1, false, [get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        auto port_obj = get_port(args[0]);
        if (!port_obj) return False;
        return (*port_obj)->port->is_output() ? True : False;
    });

    // ========================================================================
    // Port management
    // ========================================================================

    env.register_builtin("close-port", 1, false, [get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        auto port_obj = get_port(args[0]);
        if (!port_obj) return std::unexpected(port_obj.error());

        auto result = (*port_obj)->port->close();
        if (!result) return std::unexpected(result.error());

        return Nil;
    });
}

}  // namespace eta::runtime

