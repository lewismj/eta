#pragma once

#include <cmath>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include "eta/runtime/error.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/types/hash_map.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/value_formatter.h"
#include "eta/util/json.h"

namespace eta::log::payload {

using eta::runtime::error::RuntimeError;
using eta::runtime::error::RuntimeErrorCode;
using eta::runtime::error::VMError;
using eta::runtime::memory::heap::Heap;
using eta::runtime::memory::heap::ObjectKind;
using eta::runtime::memory::intern::InternTable;
using eta::runtime::nanbox::False;
using eta::runtime::nanbox::LispVal;
using eta::runtime::nanbox::Nil;
using eta::runtime::nanbox::Tag;
using eta::runtime::nanbox::True;
namespace ops = eta::runtime::nanbox::ops;

namespace detail {

inline std::unexpected<RuntimeError> type_error(const std::string& msg) {
    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, msg}});
}

inline std::expected<std::string, RuntimeError> key_to_string(LispVal key, InternTable& intern) {
    if (auto sv = eta::runtime::StringView::try_from(key, intern)) {
        return std::string(sv->view());
    }
    if (ops::is_boxed(key) && ops::tag(key) == Tag::Symbol) {
        auto text = intern.get_string(ops::payload(key));
        if (!text) {
            return std::unexpected(RuntimeError{
                VMError{RuntimeErrorCode::InternalError, "log: unresolved symbol key"}});
        }
        return std::string(*text);
    }
    return type_error("log: payload keys must be strings or symbols");
}

inline std::expected<eta::json::Value, RuntimeError> lisp_to_json(
    LispVal value,
    Heap& heap,
    InternTable& intern) {
    if (value == Nil) return eta::json::Value(nullptr);
    if (value == True) return eta::json::Value(true);
    if (value == False) return eta::json::Value(false);

    auto numeric = eta::runtime::classify_numeric(value, heap);
    if (numeric.is_valid()) {
        if (numeric.is_flonum()) {
            if (!std::isfinite(numeric.float_val)) {
                return type_error("log: cannot encode NaN/infinity in JSON payload");
            }
            return eta::json::Value(numeric.float_val);
        }
        return eta::json::Value(numeric.int_val);
    }

    if (auto sv = eta::runtime::StringView::try_from(value, intern)) {
        return eta::json::Value(std::string(sv->view()));
    }

    if (ops::is_boxed(value) && ops::tag(value) == Tag::Symbol) {
        auto text = intern.get_string(ops::payload(value));
        if (!text) return type_error("log: unresolved symbol payload");
        return eta::json::Value(std::string(*text));
    }

    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
        return eta::json::Value(eta::runtime::format_value(value, eta::runtime::FormatMode::Write, heap, intern));
    }

    const auto id = ops::payload(value);
    if (auto* vec = heap.try_get_as<ObjectKind::Vector, eta::runtime::types::Vector>(id)) {
        eta::json::Array out;
        out.reserve(vec->elements.size());
        for (auto element : vec->elements) {
            auto mapped = lisp_to_json(element, heap, intern);
            if (!mapped) return std::unexpected(mapped.error());
            out.push_back(std::move(*mapped));
        }
        return eta::json::Value(std::move(out));
    }

    if (auto* map = heap.try_get_as<ObjectKind::HashMap, eta::runtime::types::HashMap>(id)) {
        eta::json::Object out;
        for (std::size_t i = 0; i < map->state.size(); ++i) {
            if (map->state[i] != static_cast<std::uint8_t>(eta::runtime::types::HashSlotState::Occupied)) {
                continue;
            }
            auto key = key_to_string(map->keys[i], intern);
            if (!key) return std::unexpected(key.error());
            auto mapped = lisp_to_json(map->values[i], heap, intern);
            if (!mapped) return std::unexpected(mapped.error());
            out.insert_or_assign(std::move(*key), std::move(*mapped));
        }
        return eta::json::Value(std::move(out));
    }

    if (auto* cons = heap.try_get_as<ObjectKind::Cons, eta::runtime::types::Cons>(id)) {
        eta::json::Array out;
        LispVal cur = value;
        while (cur != Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
                return eta::json::Value(eta::runtime::format_value(
                    value, eta::runtime::FormatMode::Write, heap, intern));
            }
            auto* cell = heap.try_get_as<ObjectKind::Cons, eta::runtime::types::Cons>(ops::payload(cur));
            if (!cell) {
                return eta::json::Value(eta::runtime::format_value(
                    value, eta::runtime::FormatMode::Write, heap, intern));
            }
            auto mapped = lisp_to_json(cell->car, heap, intern);
            if (!mapped) return std::unexpected(mapped.error());
            out.push_back(std::move(*mapped));
            cur = cell->cdr;
        }
        (void)cons;
        return eta::json::Value(std::move(out));
    }

    return eta::json::Value(eta::runtime::format_value(value, eta::runtime::FormatMode::Write, heap, intern));
}

inline std::expected<std::vector<std::pair<std::string, LispVal>>, RuntimeError> extract_fields(
    LispVal payload,
    Heap& heap,
    InternTable& intern) {
    std::vector<std::pair<std::string, LispVal>> out;
    if (payload == Nil) return out;

    if (ops::is_boxed(payload) && ops::tag(payload) == Tag::HeapObject) {
        const auto id = ops::payload(payload);
        if (auto* map = heap.try_get_as<ObjectKind::HashMap, eta::runtime::types::HashMap>(id)) {
            out.reserve(map->state.size());
            for (std::size_t i = 0; i < map->state.size(); ++i) {
                if (map->state[i] != static_cast<std::uint8_t>(eta::runtime::types::HashSlotState::Occupied)) continue;
                auto key = key_to_string(map->keys[i], intern);
                if (!key) return std::unexpected(key.error());
                out.emplace_back(std::move(*key), map->values[i]);
            }
            return out;
        }

        if (heap.try_get_as<ObjectKind::Cons, eta::runtime::types::Cons>(id)) {
            LispVal cur = payload;
            while (cur != Nil) {
                if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
                    return type_error("log: payload alist must be a proper list");
                }
                auto* cell = heap.try_get_as<ObjectKind::Cons, eta::runtime::types::Cons>(ops::payload(cur));
                if (!cell) return type_error("log: payload alist must be a proper list");
                if (!ops::is_boxed(cell->car) || ops::tag(cell->car) != Tag::HeapObject) {
                    return type_error("log: payload entries must be pairs");
                }
                auto* pair = heap.try_get_as<ObjectKind::Cons, eta::runtime::types::Cons>(ops::payload(cell->car));
                if (!pair) return type_error("log: payload entries must be pairs");
                auto key = key_to_string(pair->car, intern);
                if (!key) return std::unexpected(key.error());
                out.emplace_back(std::move(*key), pair->cdr);
                cur = cell->cdr;
            }
            return out;
        }
    }

    return type_error("log: payload must be an alist, hash-map, or '()");
}

} // namespace detail

inline std::expected<std::string, RuntimeError> render_human(
    const std::string& msg,
    LispVal payload,
    Heap& heap,
    InternTable& intern) {
    auto fields = detail::extract_fields(payload, heap, intern);
    if (!fields) return std::unexpected(fields.error());

    std::string rendered = msg;
    for (const auto& [key, value] : *fields) {
        rendered += " ";
        rendered += key;
        rendered += "=";
        rendered += eta::runtime::format_value(value, eta::runtime::FormatMode::Write, heap, intern);
    }
    return rendered;
}

inline std::expected<std::string, RuntimeError> render_json(
    const std::string& logger_name,
    const std::string& level_name,
    const std::string& msg,
    LispVal payload,
    Heap& heap,
    InternTable& intern) {
    auto fields = detail::extract_fields(payload, heap, intern);
    if (!fields) return std::unexpected(fields.error());

    eta::json::Object obj;
    obj.insert_or_assign("level", eta::json::Value(level_name));
    obj.insert_or_assign("logger", eta::json::Value(logger_name));
    obj.insert_or_assign("msg", eta::json::Value(msg));

    for (const auto& [key, value] : *fields) {
        auto mapped = detail::lisp_to_json(value, heap, intern);
        if (!mapped) return std::unexpected(mapped.error());
        if (!obj.contains(key)) {
            obj.insert_or_assign(key, std::move(*mapped));
        } else {
            obj.insert_or_assign("payload." + key, std::move(*mapped));
        }
    }
    return eta::json::to_string(eta::json::Value(std::move(obj)));
}

} ///< namespace eta::log::payload
