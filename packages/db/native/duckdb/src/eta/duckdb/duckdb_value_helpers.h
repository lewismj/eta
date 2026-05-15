#pragma once

/**
 * @file duckdb_value_helpers.h
 * @brief Local conversion helpers for DuckDB sidecar primitive bindings.
 */

#include "eta/native/runtime_binding.h"
#include "eta/native/sdk.h"
#include "eta/runtime/error.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/types/cons.h"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace eta::duckdb_sidecar::detail {

using eta::native::SidecarRuntimeBindingV1;
using eta::runtime::error::RuntimeError;
using eta::runtime::error::RuntimeErrorCode;
using eta::runtime::error::VMError;
using eta::runtime::memory::heap::Heap;
using eta::runtime::memory::intern::InternTable;
using eta::runtime::nanbox::LispVal;

/**
 * @brief Runtime handles required for sidecar value conversion.
 */
struct RuntimeBinding {
    Heap* heap{nullptr};
    InternTable* intern_table{nullptr};
};

/**
 * @brief Build a runtime type error.
 */
[[nodiscard]] inline RuntimeError type_error(std::string message) {
    return RuntimeError{VMError{RuntimeErrorCode::TypeError, std::move(message)}};
}

/**
 * @brief Build a runtime user error.
 */
[[nodiscard]] inline RuntimeError user_error(std::string message) {
    return RuntimeError{VMError{RuntimeErrorCode::UserError, std::move(message)}};
}

/**
 * @brief Build a runtime internal error.
 */
[[nodiscard]] inline RuntimeError internal_error(std::string message) {
    return RuntimeError{VMError{RuntimeErrorCode::InternalError, std::move(message)}};
}

/**
 * @brief Decode sidecar runtime binding pointers from the native API table.
 */
[[nodiscard]] inline std::expected<RuntimeBinding, RuntimeError> runtime_binding_from_api(
    const EtaNativeApiV1* api,
    const char* who) {
    if (api == nullptr || api->runtime_context == nullptr) {
        return std::unexpected(internal_error(
            std::string(who) + ": runtime context is unavailable"));
    }

    auto* binding = static_cast<SidecarRuntimeBindingV1*>(api->runtime_context);
    if (binding->heap == nullptr || binding->intern_table == nullptr) {
        return std::unexpected(internal_error(
            std::string(who) + ": runtime heap/intern table is unavailable"));
    }

    return RuntimeBinding{
        .heap = binding->heap,
        .intern_table = binding->intern_table,
    };
}

/**
 * @brief Require a string argument and materialize an owned copy.
 */
[[nodiscard]] inline std::expected<std::string, RuntimeError> require_string_arg(
    RuntimeBinding& runtime,
    const LispVal value,
    const char* who,
    const char* arg_label) {
    auto sv = eta::runtime::StringView::try_from(value, *runtime.intern_table);
    if (!sv) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " must be a string"));
    }
    return std::string(sv->view());
}

/**
 * @brief Require a symbol-or-string argument and return text.
 */
[[nodiscard]] inline std::expected<std::string, RuntimeError> require_symbol_or_string_arg(
    RuntimeBinding& runtime,
    const LispVal value,
    const char* who,
    const char* arg_label) {
    using namespace eta::runtime::nanbox;
    if (ops::is_boxed(value) && ops::tag(value) == Tag::Symbol) {
        auto interned = runtime.intern_table->get_string(ops::payload(value));
        if (!interned) {
            return std::unexpected(internal_error(
                std::string(who) + ": unresolved symbol payload"));
        }
        return std::string(*interned);
    }
    return require_string_arg(runtime, value, who, arg_label);
}

/**
 * @brief Require an integer argument represented as Eta numeric.
 */
[[nodiscard]] inline std::expected<std::int64_t, RuntimeError> require_integer_arg(
    RuntimeBinding& runtime,
    const LispVal value,
    const char* who,
    const char* arg_label) {
    const auto numeric = eta::runtime::classify_numeric(value, *runtime.heap);
    if (!numeric.is_valid() || numeric.is_flonum()) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " must be an integer"));
    }
    return numeric.int_val;
}

/**
 * @brief Allocate an Eta string value.
 */
[[nodiscard]] inline std::expected<LispVal, RuntimeError> make_eta_string(
    RuntimeBinding& runtime,
    std::string_view text) {
    auto value =
        eta::runtime::memory::factory::make_string(*runtime.heap, *runtime.intern_table, std::string(text));
    if (!value) return std::unexpected(value.error());
    return *value;
}

/**
 * @brief Allocate an Eta symbol value.
 */
[[nodiscard]] inline std::expected<LispVal, RuntimeError> make_eta_symbol(
    RuntimeBinding& runtime,
    std::string_view text) {
    auto value = eta::runtime::memory::factory::make_symbol(*runtime.intern_table, std::string(text));
    if (!value) return std::unexpected(value.error());
    return *value;
}

/**
 * @brief Allocate an Eta cons pair.
 */
[[nodiscard]] inline std::expected<LispVal, RuntimeError> make_eta_cons(
    RuntimeBinding& runtime,
    const LispVal car,
    const LispVal cdr) {
    auto value = eta::runtime::memory::factory::make_cons(*runtime.heap, car, cdr);
    if (!value) return std::unexpected(value.error());
    return *value;
}

/**
 * @brief Encode an Eta integer value.
 */
[[nodiscard]] inline std::expected<LispVal, RuntimeError> make_eta_fixnum(
    RuntimeBinding& runtime,
    const std::int64_t value) {
    auto encoded = eta::runtime::memory::factory::make_fixnum(*runtime.heap, value);
    if (!encoded) return std::unexpected(encoded.error());
    return *encoded;
}

/**
 * @brief Encode an Eta floating-point value.
 */
[[nodiscard]] inline std::expected<LispVal, RuntimeError> make_eta_flonum(const double value) {
    auto encoded = eta::runtime::memory::factory::make_flonum(value);
    if (!encoded) return std::unexpected(encoded.error());
    return *encoded;
}

} // namespace eta::duckdb_sidecar::detail

