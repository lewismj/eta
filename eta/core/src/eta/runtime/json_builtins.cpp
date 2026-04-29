#include "eta/runtime/json_builtins.h"

#include <exception>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "eta/runtime/error.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/port.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/types/hash_map.h"
#include "eta/runtime/types/types.h"
#include "eta/util/json.h"

namespace eta::runtime {
using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::error;

namespace {
using Args = std::span<const LispVal>;

inline constexpr const char* kJsonErrorTag = "runtime.json-error";

std::unexpected<RuntimeError> type_error(std::string message) {
    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, std::move(message)}});
}

std::unexpected<RuntimeError> json_parse_error(const char* who, std::string detail) {
    VMError err{
        RuntimeErrorCode::UserError,
        std::string(who) + ": invalid JSON: " + std::move(detail)
    };
    err.tag_override = kJsonErrorTag;
    return std::unexpected(RuntimeError{std::move(err)});
}

std::expected<std::string, RuntimeError> require_string(
    InternTable& intern_table,
    LispVal value,
    const char* who,
    const char* position_label) {
    auto sv = StringView::try_from(value, intern_table);
    if (!sv) {
        return type_error(std::string(who) + ": " + position_label + " must be a string");
    }
    return std::string(sv->view());
}

std::expected<bool, RuntimeError> require_bool(LispVal value, const char* who, const char* label) {
    if (value == True) return true;
    if (value == False) return false;
    return type_error(std::string(who) + ": " + label + " must be #t or #f");
}

std::expected<types::PortObject*, RuntimeError> require_port(
    Heap& heap,
    LispVal value,
    const char* who,
    bool require_input,
    bool require_output) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
        return type_error(std::string(who) + ": expected a port");
    }
    auto* port_obj = heap.try_get_as<ObjectKind::Port, types::PortObject>(ops::payload(value));
    if (!port_obj) {
        return type_error(std::string(who) + ": expected a port");
    }
    if (require_input && !port_obj->port->is_input()) {
        return type_error(std::string(who) + ": expected an input port");
    }
    if (require_output && !port_obj->port->is_output()) {
        return type_error(std::string(who) + ": expected an output port");
    }
    return port_obj;
}

std::string read_all_chars_from_port(Port& port) {
    std::string out;
    while (true) {
        auto ch = port.read_char();
        if (!ch.has_value()) break;
        out += utf8::encode(*ch);
    }
    return out;
}

std::expected<json::Value, RuntimeError> parse_json_text(const char* who, std::string_view text) {
    try {
        return json::parse(text);
    } catch (const json::ParseError& ex) {
        return json_parse_error(who, ex.what());
    } catch (const std::exception& ex) {
        return json_parse_error(who, ex.what());
    }
}

std::expected<LispVal, RuntimeError> call_builtin(
    BuiltinEnvironment& env,
    const char* name,
    const std::vector<LispVal>& args) {
    auto idx = env.lookup(name);
    if (!idx) {
        return std::unexpected(RuntimeError{
            VMError{RuntimeErrorCode::InternalError, std::string("missing builtin: ") + name}});
    }
    return env.specs()[*idx].func(args);
}

std::expected<LispVal, RuntimeError> json_to_lisp(
    const json::Value& value,
    bool keep_integers_exact,
    BuiltinEnvironment& env,
    Heap& heap,
    InternTable& intern_table) {
    if (value.is_null()) return Nil;
    if (value.is_bool()) return value.as_bool() ? True : False;
    if (value.is_int()) {
        if (keep_integers_exact) {
            return make_fixnum(heap, value.as_int());
        }
        return make_flonum(static_cast<double>(value.as_int()));
    }
    if (value.is_double()) {
        return make_flonum(value.as_double());
    }
    if (value.is_string()) {
        return make_string(heap, intern_table, value.as_string());
    }
    if (value.is_array()) {
        std::vector<LispVal> elems;
        elems.reserve(value.as_array().size());
        for (const auto& item : value.as_array()) {
            auto mapped = json_to_lisp(item, keep_integers_exact, env, heap, intern_table);
            if (!mapped) return std::unexpected(mapped.error());
            elems.push_back(*mapped);
        }
        return make_vector(heap, std::move(elems));
    }
    if (value.is_object()) {
        std::vector<LispVal> kv_args;
        kv_args.reserve(value.as_object().size() * 2);
        for (const auto& [k, v] : value.as_object()) {
            auto key_val = make_string(heap, intern_table, k);
            if (!key_val) return std::unexpected(key_val.error());
            auto mapped = json_to_lisp(v, keep_integers_exact, env, heap, intern_table);
            if (!mapped) return std::unexpected(mapped.error());
            kv_args.push_back(*key_val);
            kv_args.push_back(*mapped);
        }
        return call_builtin(env, "hash-map", kv_args);
    }
    return std::unexpected(RuntimeError{
        VMError{RuntimeErrorCode::InternalError, "json conversion: unknown JSON value kind"}});
}

std::expected<std::string, RuntimeError> json_key_to_string(
    InternTable& intern_table,
    LispVal key,
    const char* who) {
    auto sv = StringView::try_from(key, intern_table);
    if (sv) return std::string(sv->view());

    if (ops::is_boxed(key) && ops::tag(key) == Tag::Symbol) {
        auto text = intern_table.get_string(ops::payload(key));
        if (!text) {
            return std::unexpected(RuntimeError{
                VMError{RuntimeErrorCode::InternalError, std::string(who) + ": unresolved symbol id"}});
        }
        return std::string(*text);
    }

    return type_error(std::string(who) + ": object keys must be strings or symbols");
}

std::expected<json::Value, RuntimeError> lisp_to_json(
    LispVal value,
    Heap& heap,
    InternTable& intern_table,
    const char* who) {
    if (value == Nil) return json::Value(nullptr);
    if (value == True) return json::Value(true);
    if (value == False) return json::Value(false);

    auto numeric = classify_numeric(value, heap);
    if (numeric.is_valid()) {
        if (numeric.is_flonum()) {
            if (!std::isfinite(numeric.float_val)) {
                return type_error(std::string(who) + ": cannot encode NaN or infinities");
            }
            return json::Value(numeric.float_val);
        }
        return json::Value(numeric.int_val);
    }

    auto sv = StringView::try_from(value, intern_table);
    if (sv) return json::Value(std::string(sv->view()));

    if (ops::is_boxed(value) && ops::tag(value) == Tag::Symbol) {
        auto text = intern_table.get_string(ops::payload(value));
        if (!text) {
            return std::unexpected(RuntimeError{
                VMError{RuntimeErrorCode::InternalError, std::string(who) + ": unresolved symbol id"}});
        }
        return json::Value(std::string(*text));
    }

    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
        return type_error(std::string(who) + ": unsupported value type for JSON encoding");
    }

    const auto obj_id = ops::payload(value);

    if (auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(obj_id)) {
        json::Array out;
        out.reserve(vec->elements.size());
        for (auto item : vec->elements) {
            auto mapped = lisp_to_json(item, heap, intern_table, who);
            if (!mapped) return std::unexpected(mapped.error());
            out.push_back(std::move(*mapped));
        }
        return json::Value(std::move(out));
    }

    if (auto* map = heap.try_get_as<ObjectKind::HashMap, types::HashMap>(obj_id)) {
        json::Object out;
        for (std::size_t i = 0; i < map->state.size(); ++i) {
            if (map->state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) {
                continue;
            }
            auto key = json_key_to_string(intern_table, map->keys[i], who);
            if (!key) return std::unexpected(key.error());
            auto mapped = lisp_to_json(map->values[i], heap, intern_table, who);
            if (!mapped) return std::unexpected(mapped.error());
            out.insert_or_assign(std::move(*key), std::move(*mapped));
        }
        return json::Value(std::move(out));
    }

    return type_error(std::string(who) + ": unsupported value type for JSON encoding");
}

} // namespace

void register_json_builtins(BuiltinEnvironment& env,
                            Heap& heap,
                            InternTable& intern_table) {
    env.register_builtin("%json-read", 2, false,
        [&env, &heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto port_obj = require_port(heap, args[0], "%json-read", true, false);
            if (!port_obj) return std::unexpected(port_obj.error());

            auto keep_exact = require_bool(args[1], "%json-read", "second argument");
            if (!keep_exact) return std::unexpected(keep_exact.error());

            const std::string text = read_all_chars_from_port(*(*port_obj)->port);
            auto parsed = parse_json_text("%json-read", text);
            if (!parsed) return std::unexpected(parsed.error());

            return json_to_lisp(*parsed, *keep_exact, env, heap, intern_table);
        });

    env.register_builtin("%json-read-string", 2, false,
        [&env, &heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto input = require_string(intern_table, args[0], "%json-read-string", "first argument");
            if (!input) return std::unexpected(input.error());

            auto keep_exact = require_bool(args[1], "%json-read-string", "second argument");
            if (!keep_exact) return std::unexpected(keep_exact.error());

            auto parsed = parse_json_text("%json-read-string", *input);
            if (!parsed) return std::unexpected(parsed.error());

            return json_to_lisp(*parsed, *keep_exact, env, heap, intern_table);
        });

    env.register_builtin("%json-write", 2, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto port_obj = require_port(heap, args[1], "%json-write", false, true);
            if (!port_obj) return std::unexpected(port_obj.error());

            auto encoded = lisp_to_json(args[0], heap, intern_table, "%json-write");
            if (!encoded) return std::unexpected(encoded.error());

            auto write_res = (*port_obj)->port->write_string(json::to_string(*encoded));
            if (!write_res) return std::unexpected(write_res.error());

            auto flush_res = (*port_obj)->port->flush();
            if (!flush_res) return std::unexpected(flush_res.error());
            return Nil;
        });

    env.register_builtin("%json-write-string", 1, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto encoded = lisp_to_json(args[0], heap, intern_table, "%json-write-string");
            if (!encoded) return std::unexpected(encoded.error());
            return make_string(heap, intern_table, json::to_string(*encoded));
        });
}

} // namespace eta::runtime
