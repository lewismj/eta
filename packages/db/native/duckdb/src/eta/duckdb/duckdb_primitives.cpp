#include "eta/duckdb/duckdb_primitives.h"

#include "eta/runtime/error.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/types/primitive.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace eta::duckdb_sidecar {

namespace {

using PrimitiveArgs = eta::runtime::types::PrimitiveArgs;
using PrimitiveFunc = eta::runtime::types::PrimitiveFunc;
using PrimitiveResult = std::expected<eta::runtime::nanbox::LispVal, eta::runtime::error::RuntimeError>;
using eta::runtime::error::RuntimeError;
using eta::runtime::error::RuntimeErrorCode;
using eta::runtime::error::VMError;
using eta::runtime::nanbox::False;
using eta::runtime::nanbox::LispVal;
using eta::runtime::nanbox::Nil;
using eta::runtime::nanbox::Tag;
using eta::runtime::nanbox::True;
using eta::runtime::nanbox::ops::decode;
using eta::runtime::nanbox::ops::encode;
using eta::runtime::nanbox::ops::is_boxed;
using eta::runtime::nanbox::ops::tag;

struct ConnectionState {
    bool open{true};
    std::string last_error;
};

std::mutex g_state_mutex;
std::int64_t g_next_connection_id = 1;
std::string g_last_error;
std::unordered_map<std::int64_t, ConnectionState> g_connections;

[[nodiscard]] RuntimeError type_error(std::string message) {
    return RuntimeError{VMError{RuntimeErrorCode::TypeError, std::move(message)}};
}

[[nodiscard]] RuntimeError user_error(std::string message) {
    return RuntimeError{VMError{RuntimeErrorCode::UserError, std::move(message)}};
}

[[nodiscard]] RuntimeError internal_error(std::string message) {
    return RuntimeError{VMError{RuntimeErrorCode::InternalError, std::move(message)}};
}

void set_last_error_locked(const std::optional<std::int64_t> connection_id,
                           std::string message) {
    g_last_error = message;
    if (connection_id.has_value()) {
        auto found = g_connections.find(*connection_id);
        if (found != g_connections.end()) {
            found->second.last_error = std::move(message);
        }
    }
}

void clear_last_error_locked(const std::optional<std::int64_t> connection_id) {
    g_last_error.clear();
    if (connection_id.has_value()) {
        auto found = g_connections.find(*connection_id);
        if (found != g_connections.end()) {
            found->second.last_error.clear();
        }
    }
}

[[nodiscard]] std::expected<std::int64_t, RuntimeError> decode_connection_id(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    auto decoded = decode<std::int64_t>(value);
    if (!decoded) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " must be a connection handle"));
    }
    return *decoded;
}

[[nodiscard]] std::expected<void, RuntimeError> expect_string_arg(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    if (!is_boxed(value) || tag(value) != Tag::String) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " must be a string"));
    }
    return {};
}

[[nodiscard]] bool is_numeric_value(const LispVal value) {
    if (!is_boxed(value)) return true;
    const Tag value_tag = tag(value);
    return value_tag == Tag::Fixnum || value_tag == Tag::Nan;
}

PrimitiveResult primitive_open(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("duckdb-open: expected path argument"));
    }

    auto path_ok = expect_string_arg(args[0], "duckdb-open", "first argument");
    if (!path_ok) return std::unexpected(path_ok.error());

    std::int64_t connection_id = 0;
    {
        const std::lock_guard<std::mutex> guard(g_state_mutex);
        connection_id = g_next_connection_id++;
        g_connections.emplace(
            connection_id,
            ConnectionState{
                .open = true,
                .last_error = {},
            });
        clear_last_error_locked(connection_id);
    }

    auto boxed = encode(connection_id);
    if (!boxed) {
        return std::unexpected(internal_error(
            "duckdb-open: failed to encode connection handle"));
    }
    return *boxed;
}

PrimitiveResult primitive_close(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("duckdb-close!: expected connection argument"));
    }

    auto connection_id = decode_connection_id(
        args[0],
        "duckdb-close!",
        "first argument");
    if (!connection_id) return std::unexpected(connection_id.error());

    const std::lock_guard<std::mutex> guard(g_state_mutex);
    auto found = g_connections.find(*connection_id);
    if (found == g_connections.end()) {
        set_last_error_locked(
            std::nullopt,
            "duckdb-close!: unknown connection handle");
        return std::unexpected(type_error("duckdb-close!: unknown connection handle"));
    }

    if (!found->second.open) return False;
    found->second.open = false;
    clear_last_error_locked(*connection_id);
    return True;
}

PrimitiveResult primitive_exec(const PrimitiveArgs args) {
    if (args.size() != 2u) {
        return std::unexpected(type_error("duckdb-exec: expected connection and SQL"));
    }

    auto connection_id = decode_connection_id(
        args[0],
        "duckdb-exec",
        "first argument");
    if (!connection_id) return std::unexpected(connection_id.error());

    auto sql_ok = expect_string_arg(
        args[1],
        "duckdb-exec",
        "second argument");
    if (!sql_ok) return std::unexpected(sql_ok.error());

    const std::lock_guard<std::mutex> guard(g_state_mutex);
    auto found = g_connections.find(*connection_id);
    if (found == g_connections.end()) {
        set_last_error_locked(
            std::nullopt,
            "duckdb-exec: unknown connection handle");
        return std::unexpected(type_error("duckdb-exec: unknown connection handle"));
    }
    if (!found->second.open) {
        set_last_error_locked(
            *connection_id,
            "duckdb-exec: connection is closed");
        return std::unexpected(user_error("duckdb-exec: connection is closed"));
    }

    clear_last_error_locked(*connection_id);
    return True;
}

PrimitiveResult primitive_query(const PrimitiveArgs args) {
    if (args.size() < 2u) {
        return std::unexpected(type_error(
            "duckdb-query: expected connection, SQL, and optional parameters"));
    }

    auto connection_id = decode_connection_id(
        args[0],
        "duckdb-query",
        "first argument");
    if (!connection_id) return std::unexpected(connection_id.error());

    auto sql_ok = expect_string_arg(
        args[1],
        "duckdb-query",
        "second argument");
    if (!sql_ok) return std::unexpected(sql_ok.error());

    const std::lock_guard<std::mutex> guard(g_state_mutex);
    auto found = g_connections.find(*connection_id);
    if (found == g_connections.end()) {
        set_last_error_locked(
            std::nullopt,
            "duckdb-query: unknown connection handle");
        return std::unexpected(type_error("duckdb-query: unknown connection handle"));
    }
    if (!found->second.open) {
        set_last_error_locked(
            *connection_id,
            "duckdb-query: connection is closed");
        return std::unexpected(user_error("duckdb-query: connection is closed"));
    }

    LispVal result = Nil;
    if (args.size() >= 3u) {
        if (!is_numeric_value(args[2])) {
            set_last_error_locked(
                *connection_id,
                "duckdb-query: first parameter must be numeric");
            return std::unexpected(type_error(
                "duckdb-query: first parameter must be numeric"));
        }
        result = args[2];
    } else {
        auto one = encode<std::int64_t>(1);
        if (!one) {
            return std::unexpected(internal_error(
                "duckdb-query: failed to encode query result"));
        }
        result = *one;
    }

    clear_last_error_locked(*connection_id);
    return result;
}

PrimitiveResult primitive_last_error(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error(
            "duckdb-last-error: expected connection handle or nil"));
    }

    std::string message;
    {
        const std::lock_guard<std::mutex> guard(g_state_mutex);
        if (args[0] == Nil) {
            message = g_last_error;
        } else {
            auto connection_id = decode_connection_id(
                args[0],
                "duckdb-last-error",
                "first argument");
            if (!connection_id) return std::unexpected(connection_id.error());
            auto found = g_connections.find(*connection_id);
            if (found == g_connections.end()) {
                message = g_last_error;
            } else {
                message = found->second.last_error;
            }
        }
    }

    (void)message;
    return Nil;
}

PrimitiveFunc g_open = primitive_open;
PrimitiveFunc g_close = primitive_close;
PrimitiveFunc g_exec = primitive_exec;
PrimitiveFunc g_query = primitive_query;
PrimitiveFunc g_last_error_primitive = primitive_last_error;

int register_one(const EtaNativeApiV1* api,
                 const char* name,
                 const std::uint32_t arity,
                 const std::uint8_t has_rest,
                 void* callable) {
    if (api == nullptr || api->register_primitive == nullptr) {
        return ETA_NATIVE_STATUS_ERROR;
    }
    return api->register_primitive(api->user_data, name, arity, has_rest, callable);
}

} // namespace

int register_duckdb_primitives(const EtaNativeApiV1* api) {
    if (api == nullptr || api->register_primitive == nullptr) {
        if (api != nullptr && api->report_error != nullptr) {
            api->report_error(
                api->user_data,
                "duckdb sidecar requires register_primitive callback support");
        }
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (register_one(api, "%duckdb-open", 1u, 0u, static_cast<void*>(&g_open))
        != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (register_one(api, "%duckdb-close!", 1u, 0u, static_cast<void*>(&g_close))
        != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (register_one(api, "%duckdb-exec", 2u, 0u, static_cast<void*>(&g_exec))
        != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (register_one(api, "%duckdb-query", 2u, 1u, static_cast<void*>(&g_query))
        != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (register_one(
            api,
            "%duckdb-last-error",
            1u,
            0u,
            static_cast<void*>(&g_last_error_primitive))
        != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    return ETA_NATIVE_STATUS_OK;
}

} // namespace eta::duckdb_sidecar
