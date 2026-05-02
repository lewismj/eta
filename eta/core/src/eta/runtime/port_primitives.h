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
#include "eta/runtime/numeric_value.h"
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
 *   - close-input-port
 *   - close-output-port
 *   - write-char
 */
inline void register_port_primitives(BuiltinEnvironment& env, Heap& heap, InternTable& intern_table, vm::VM& vm) {
    using Args = std::span<const LispVal>;

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

    /**
     * Current port accessors
     */

    env.register_builtin("current-input-port", 0, false, [&vm](Args) -> std::expected<LispVal, RuntimeError> {
        return vm.current_input_port();
    });

    env.register_builtin("current-output-port", 0, false, [&vm](Args) -> std::expected<LispVal, RuntimeError> {
        return vm.current_output_port();
    });

    env.register_builtin("current-error-port", 0, false, [&vm](Args) -> std::expected<LispVal, RuntimeError> {
        return vm.current_error_port();
    });

    /**
     * Current port setters
     */

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

    /**
     * String ports
     */

    env.register_builtin("open-output-string", 0, false, [&heap](Args) -> std::expected<LispVal, RuntimeError> {
        auto port = std::make_shared<StringPort>(StringPort::Mode::Output);
        return make_port(heap, port);
    });

    env.register_builtin("get-output-string", 1, false, [&heap, &intern_table, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        auto port_obj = get_port(args[0]);
        if (!port_obj) return std::unexpected(port_obj.error());

        /// Try to downcast to StringPort
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

    /**
     * Port I/O operations
     */

    env.register_builtin("write-string", 1, true, [&vm, &intern_table, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "write-string: requires at least 1 argument"}});
        }

        auto sv = StringView::try_from(args[0], intern_table);
        if (!sv) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "write-string: first argument must be a string"}});
        }

        /// Optional port argument (defaults to current-output-port)
        LispVal port_val = args.size() > 1 ? args[1] : vm.current_output_port();

        auto port_obj = get_port(port_val);
        if (!port_obj) return std::unexpected(port_obj.error());

        auto result = (*port_obj)->port->write_string(std::string(sv->view()));
        if (!result) return std::unexpected(result.error());

        return Nil;
    });

    env.register_builtin("read-char", 0, true, [&heap, &vm, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        /// Optional port argument (defaults to current-input-port)
        LispVal port_val = args.empty() ? vm.current_input_port() : args[0];

        auto port_obj = get_port(port_val);
        if (!port_obj) return std::unexpected(port_obj.error());

        auto ch = (*port_obj)->port->read_char();
        if (!ch) {
            /// EOF - return a special EOF object (we'll use False for now, Scheme typically uses an eof-object)
            return False;
        }

        auto encoded = ops::encode(*ch);
        if (!encoded) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "read-char: failed to encode character"}});
        }

        return *encoded;
    });

    /**
     * Port predicates
     */

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

    /**
     * Port management
     */

    env.register_builtin("close-port", 1, false, [get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        auto port_obj = get_port(args[0]);
        if (!port_obj) return std::unexpected(port_obj.error());

        auto result = (*port_obj)->port->close();
        if (!result) return std::unexpected(result.error());

        return Nil;
    });

    /// Standard Scheme aliases for close-port
    env.register_builtin("close-input-port", 1, false, [get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        auto port_obj = get_port(args[0]);
        if (!port_obj) return std::unexpected(port_obj.error());
        if (!(*port_obj)->port->is_input()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "close-input-port: not an input port"}});
        }
        auto result = (*port_obj)->port->close();
        if (!result) return std::unexpected(result.error());
        return Nil;
    });

    env.register_builtin("close-output-port", 1, false, [get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        auto port_obj = get_port(args[0]);
        if (!port_obj) return std::unexpected(port_obj.error());
        if (!(*port_obj)->port->is_output()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "close-output-port: not an output port"}});
        }
        auto result = (*port_obj)->port->close();
        if (!result) return std::unexpected(result.error());
        return Nil;
    });

    /**
     * Character I/O
     */

    env.register_builtin("write-char", 1, true, [&vm, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "write-char: requires at least 1 argument"}});
        }

        /// First argument must be a character
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::Char) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "write-char: first argument must be a character"}});
        }
        auto ch = ops::decode<char32_t>(args[0]);
        if (!ch) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "write-char: invalid character"}});
        }

        /// Encode the character as UTF-8
        std::string utf8;
        char32_t c = *ch;
        if (c < 0x80) {
            utf8 += static_cast<char>(c);
        } else if (c < 0x800) {
            utf8 += static_cast<char>(0xC0 | (c >> 6));
            utf8 += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            utf8 += static_cast<char>(0xE0 | (c >> 12));
            utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            utf8 += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            utf8 += static_cast<char>(0xF0 | (c >> 18));
            utf8 += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            utf8 += static_cast<char>(0x80 | (c & 0x3F));
        }

        /// Optional port argument (defaults to current-output-port)
        LispVal port_val = args.size() > 1 ? args[1] : vm.current_output_port();

        auto port_obj = get_port(port_val);
        if (!port_obj) return std::unexpected(port_obj.error());

        auto result = (*port_obj)->port->write_string(utf8);
        if (!result) return std::unexpected(result.error());

        return Nil;
    });

    /**
     * File port operations
     */

    env.register_builtin("open-input-file", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto sv = StringView::try_from(args[0], intern_table);
        if (!sv) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "open-input-file: not a string"}});
        }

        auto port = std::make_shared<FilePort>(std::string(sv->view()), FilePort::Mode::Read);
        if (!port->is_open()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "open-input-file: could not open file"}});
        }
        return make_port(heap, port);
    });

    env.register_builtin("open-output-file", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto sv = StringView::try_from(args[0], intern_table);
        if (!sv) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "open-output-file: not a string"}});
        }

        auto port = std::make_shared<FilePort>(std::string(sv->view()), FilePort::Mode::Write);
        if (!port->is_open()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "open-output-file: could not open file"}});
        }
        return make_port(heap, port);
    });

    /**
     * Binary port operations
     */

    env.register_builtin("open-output-bytevector", 0, false, [&heap](Args) -> std::expected<LispVal, RuntimeError> {
        auto port = std::make_shared<BinaryPort>(BinaryPort::Mode::Output);
        return make_port(heap, port);
    });

    env.register_builtin("open-input-bytevector", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "open-input-bytevector: not a bytevector"}});
        }
        auto* bv = heap.try_get_as<ObjectKind::ByteVector, types::ByteVector>(ops::payload(args[0]));
        if (!bv) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "open-input-bytevector: not a bytevector"}});
        }

        auto port = std::make_shared<BinaryPort>(BinaryPort::Mode::Input, bv->data);
        return make_port(heap, port);
    });

    env.register_builtin("get-output-bytevector", 1, false, [&heap, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        auto port_obj = get_port(args[0]);
        if (!port_obj) return std::unexpected(port_obj.error());

        /// Try to downcast to BinaryPort
        auto* binary_port = dynamic_cast<BinaryPort*>((*port_obj)->port.get());
        if (!binary_port) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "get-output-bytevector: not a binary port"}});
        }

        return make_bytevector(heap, binary_port->get_bytes());
    });

    env.register_builtin("read-u8", 0, true, [&vm, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        /// Optional port argument (defaults to current-input-port)
        LispVal port_val = args.empty() ? vm.current_input_port() : args[0];

        auto port_obj = get_port(port_val);
        if (!port_obj) return std::unexpected(port_obj.error());

        auto* binary_port = dynamic_cast<BytePort*>((*port_obj)->port.get());
        if (!binary_port) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "read-u8: not a binary port"}});
        }

        auto byte = binary_port->read_byte();
        if (!byte) {
            return False;  ///< EOF
        }

        return ops::encode(static_cast<int64_t>(*byte)).value();
    });

    env.register_builtin("write-u8", 1, true, [&vm, &heap, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "write-u8: requires at least 1 argument"}});
        }

        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid() || n.is_flonum() || n.int_val < 0 || n.int_val > 255) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "write-u8: argument must be a byte (0-255)"}});
        }

        /// Optional port argument (defaults to current-output-port)
        LispVal port_val = args.size() > 1 ? args[1] : vm.current_output_port();

        auto port_obj = get_port(port_val);
        if (!port_obj) return std::unexpected(port_obj.error());

        auto* binary_port = dynamic_cast<BytePort*>((*port_obj)->port.get());
        if (!binary_port) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "write-u8: not a binary port"}});
        }

        auto result = binary_port->write_byte(static_cast<uint8_t>(n.int_val));
        if (!result) return std::unexpected(result.error());

        return Nil;
    });

    env.register_builtin("binary-port?", 1, false, [get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        auto port_obj = get_port(args[0]);
        if (!port_obj) return False;
        return (*port_obj)->port->is_binary() ? True : False;
    });
}

}  ///< namespace eta::runtime

