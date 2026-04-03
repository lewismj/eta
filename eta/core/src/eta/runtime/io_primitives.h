#pragma once

#include <bit>
#include <expected>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/numeric_value.h"
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
 * @brief Register I/O primitives that require VM access.
 *
 * This must be called AFTER the VM is created, unlike core primitives.
 *
 * Primitives registered:
 *   - display (port-aware version)
 *   - newline (port-aware version)
 */
inline void register_io_primitives(BuiltinEnvironment& env, Heap& heap, InternTable& intern_table, vm::VM& vm) {
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
    // Port-aware I/O: display newline
    // ========================================================================

    env.register_builtin("display", 1, true, [&heap, &intern_table, &vm, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "display: requires at least 1 argument"}});
        }

        LispVal v = args[0];

        // Format value to string
        std::string output;
        if (v == nanbox::Nil) { output = "()"; }
        else if (v == nanbox::True) { output = "#t"; }
        else if (v == nanbox::False) { output = "#f"; }
        else if (!ops::is_boxed(v)) {
            // Raw double
            std::ostringstream oss;
            oss << std::bit_cast<double>(v);
            output = oss.str();
        } else {
            Tag t = ops::tag(v);
            if (t == Tag::Fixnum) {
                auto val = ops::decode<int64_t>(v);
                if (val) {
                    output = std::to_string(*val);
                }
            } else if (t == Tag::Char) {
                auto val = ops::decode<char32_t>(v);
                if (val) {
                    // Output as UTF-8
                    char32_t c = *val;
                    if (c < 0x80) {
                        output = std::string(1, static_cast<char>(c));
                    } else {
                        output = utf8::encode(c);
                    }
                }
            } else if (t == Tag::String) {
                auto sv = StringView::try_from(v, intern_table);
                if (sv) output = std::string(sv->view());
            } else if (t == Tag::Symbol) {
                auto sv = intern_table.get_string(ops::payload(v));
                if (sv) output = std::string(*sv);
            } else if (t == Tag::HeapObject) {
                auto n = classify_numeric(v, heap);
                if (n.is_fixnum()) {
                    output = std::to_string(n.int_val);
                } else if (n.is_flonum()) {
                    std::ostringstream oss;
                    oss << n.float_val;
                    output = oss.str();
                } else {
                    output = "#<object>";
                }
            } else {
                output = "#<unknown>";
            }
        }

        // Optional port argument (defaults to current-output-port)
        LispVal port_val = args.size() > 1 ? args[1] : vm.current_output_port();

        auto port_obj = get_port(port_val);
        if (!port_obj) {
            // Fallback to stdout if port is invalid
            std::cout << output;
            return nanbox::Nil;
        }

        auto result = (*port_obj)->port->write_string(output);
        if (!result) return std::unexpected(result.error());

        return nanbox::Nil;
    });

    env.register_builtin("newline", 0, true, [&vm, get_port](Args args) -> std::expected<LispVal, RuntimeError> {
        // Optional port argument (defaults to current-output-port)
        LispVal port_val = args.empty() ? vm.current_output_port() : args[0];

        auto port_obj = get_port(port_val);
        if (!port_obj) {
            // Fallback to stdout if port is invalid
            std::cout << '\n';
            return nanbox::Nil;
        }

        auto result = (*port_obj)->port->write_string("\n");
        if (!result) return std::unexpected(result.error());

        return nanbox::Nil;
    });
}

}  // namespace eta::runtime

