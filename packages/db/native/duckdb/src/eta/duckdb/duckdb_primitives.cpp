#include "eta/duckdb/duckdb_primitives.h"
#include "eta/duckdb/duckdb_value_helpers.h"

#include "eta/runtime/nanbox.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/types/primitive.h"

#include <duckdb.h>

#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace eta::duckdb_sidecar {

namespace {

using PrimitiveArgs = eta::runtime::types::PrimitiveArgs;
using PrimitiveFunc = eta::runtime::types::PrimitiveFunc;
using PrimitiveResult = std::expected<eta::runtime::nanbox::LispVal, eta::runtime::error::RuntimeError>;
using detail::RuntimeBinding;
using detail::internal_error;
using detail::make_eta_cons;
using detail::make_eta_fixnum;
using detail::make_eta_flonum;
using detail::make_eta_string;
using detail::make_eta_symbol;
using detail::require_integer_arg;
using detail::require_string_arg;
using detail::require_symbol_or_string_arg;
using detail::type_error;
using detail::user_error;
using eta::runtime::error::RuntimeError;
using eta::runtime::error::VMError;
using eta::runtime::nanbox::False;
using eta::runtime::nanbox::LispVal;
using eta::runtime::nanbox::Nil;
using eta::runtime::nanbox::Tag;
using eta::runtime::nanbox::True;
using eta::runtime::nanbox::ops::is_boxed;
using eta::runtime::nanbox::ops::tag;

struct ConnectionState {
    bool open{true};
    std::string last_error;
    duckdb_database database{nullptr};
    duckdb_connection connection{nullptr};
};

std::mutex g_state_mutex;
std::int64_t g_next_connection_id = 1;
std::string g_last_error;
std::unordered_map<std::int64_t, ConnectionState> g_connections;
std::optional<RuntimeBinding> g_runtime_binding;

void set_last_error_locked(const std::optional<std::int64_t> connection_id,
                           const std::string& message) {
    g_last_error = message;
    if (connection_id.has_value()) {
        auto found = g_connections.find(*connection_id);
        if (found != g_connections.end()) {
            found->second.last_error = message;
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

[[nodiscard]] std::string runtime_error_message(const RuntimeError& err) {
    return std::visit(
        [](const auto& value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, VMError>) {
                return value.message;
            } else {
                return "runtime error";
            }
        },
        err);
}

[[nodiscard]] std::expected<RuntimeBinding, RuntimeError> runtime_binding(const char* who) {
    if (!g_runtime_binding.has_value()) {
        return std::unexpected(internal_error(
            std::string(who) + ": runtime binding is unavailable"));
    }
    return *g_runtime_binding;
}

[[nodiscard]] std::expected<std::int64_t, RuntimeError> decode_connection_id(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    auto runtime = runtime_binding(who);
    if (!runtime) return std::unexpected(runtime.error());

    auto decoded = require_integer_arg(*runtime, value, who, arg_label);
    if (!decoded) return std::unexpected(decoded.error());
    if (*decoded <= 0) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " must be a connection handle"));
    }
    return *decoded;
}

[[nodiscard]] std::string prefixed_error(const char* who, std::string detail) {
    return std::string(who) + ": " + std::move(detail);
}

[[nodiscard]] std::string take_open_error_message(char* open_error) {
    if (open_error == nullptr) return "failed to open DuckDB database";
    const std::string message = open_error[0] == '\0'
        ? std::string("failed to open DuckDB database")
        : std::string(open_error);
    duckdb_free(open_error);
    return message;
}

[[nodiscard]] std::string result_error_message(duckdb_result* result, const char* fallback) {
    if (result == nullptr) return std::string(fallback);
    const char* error = duckdb_result_error(result);
    if (error == nullptr || error[0] == '\0') return std::string(fallback);
    return std::string(error);
}

[[nodiscard]] std::string prepare_error_message(
    const duckdb_prepared_statement statement,
    const char* fallback) {
    if (statement == nullptr) return std::string(fallback);
    const char* error = duckdb_prepare_error(statement);
    if (error == nullptr || error[0] == '\0') return std::string(fallback);
    return std::string(error);
}

void close_connection_handles(ConnectionState& state) {
    if (state.connection != nullptr) {
        duckdb_disconnect(&state.connection);
    }
    if (state.database != nullptr) {
        duckdb_close(&state.database);
    }
    state.open = false;
}

[[nodiscard]] std::expected<void, RuntimeError> bind_query_parameter(
    RuntimeBinding& runtime,
    duckdb_prepared_statement statement,
    const idx_t parameter_index,
    const LispVal value) {
    duckdb_state bind_status = DuckDBError;

    if (value == Nil) {
        bind_status = duckdb_bind_null(statement, parameter_index);
    } else if (value == True || value == False) {
        bind_status = duckdb_bind_boolean(statement, parameter_index, value == True);
    } else {
        const auto numeric = eta::runtime::classify_numeric(value, *runtime.heap);
        if (numeric.is_valid()) {
            if (numeric.is_flonum()) {
                bind_status = duckdb_bind_double(statement, parameter_index, numeric.float_val);
            } else {
                bind_status = duckdb_bind_int64(statement, parameter_index, numeric.int_val);
            }
        } else if (is_boxed(value) && (tag(value) == Tag::String || tag(value) == Tag::Symbol)) {
            std::string label = "parameter " + std::to_string(static_cast<unsigned long long>(parameter_index));
            auto text = require_symbol_or_string_arg(
                runtime,
                value,
                "duckdb-query",
                label.c_str());
            if (!text) return std::unexpected(text.error());
            bind_status = duckdb_bind_varchar(statement, parameter_index, text->c_str());
        } else {
            return std::unexpected(type_error(
                "duckdb-query: query parameters must be nil, booleans, numbers, strings, or symbols"));
        }
    }

    if (bind_status == DuckDBError) {
        return std::unexpected(user_error(prefixed_error(
            "duckdb-query",
            prepare_error_message(statement, "failed to bind query parameter"))));
    }
    return {};
}

[[nodiscard]] std::expected<LispVal, RuntimeError> decode_result_value(
    RuntimeBinding& runtime,
    duckdb_result* result,
    const idx_t column_index,
    const idx_t row_index) {
    if (duckdb_value_is_null(result, column_index, row_index)) return Nil;

    switch (duckdb_column_type(result, column_index)) {
        case DUCKDB_TYPE_BOOLEAN:
            return duckdb_value_boolean(result, column_index, row_index) ? True : False;
        case DUCKDB_TYPE_TINYINT:
            return make_eta_fixnum(runtime, static_cast<std::int64_t>(
                duckdb_value_int8(result, column_index, row_index)));
        case DUCKDB_TYPE_SMALLINT:
            return make_eta_fixnum(runtime, static_cast<std::int64_t>(
                duckdb_value_int16(result, column_index, row_index)));
        case DUCKDB_TYPE_INTEGER:
            return make_eta_fixnum(runtime, static_cast<std::int64_t>(
                duckdb_value_int32(result, column_index, row_index)));
        case DUCKDB_TYPE_BIGINT:
            return make_eta_fixnum(runtime, duckdb_value_int64(result, column_index, row_index));
        case DUCKDB_TYPE_UTINYINT:
            return make_eta_fixnum(runtime, static_cast<std::int64_t>(
                duckdb_value_uint8(result, column_index, row_index)));
        case DUCKDB_TYPE_USMALLINT:
            return make_eta_fixnum(runtime, static_cast<std::int64_t>(
                duckdb_value_uint16(result, column_index, row_index)));
        case DUCKDB_TYPE_UINTEGER:
            return make_eta_fixnum(runtime, static_cast<std::int64_t>(
                duckdb_value_uint32(result, column_index, row_index)));
        case DUCKDB_TYPE_UBIGINT: {
            const std::uint64_t value = duckdb_value_uint64(result, column_index, row_index);
            if (value <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
                return make_eta_fixnum(runtime, static_cast<std::int64_t>(value));
            }
            return make_eta_flonum(static_cast<double>(value));
        }
        case DUCKDB_TYPE_FLOAT:
            return make_eta_flonum(static_cast<double>(
                duckdb_value_float(result, column_index, row_index)));
        case DUCKDB_TYPE_DOUBLE:
        case DUCKDB_TYPE_DECIMAL:
            return make_eta_flonum(duckdb_value_double(result, column_index, row_index));
        case DUCKDB_TYPE_SQLNULL:
            return Nil;
        default: {
            char* text = duckdb_value_varchar(result, column_index, row_index);
            if (text == nullptr) {
                return std::unexpected(user_error(
                    "duckdb-query: failed to convert query result value"));
            }
            std::string owned{text};
            duckdb_free(text);
            return make_eta_string(runtime, owned);
        }
    }
}

[[nodiscard]] std::expected<LispVal, RuntimeError> materialize_rows(
    RuntimeBinding& runtime,
    duckdb_result* result) {
    const idx_t row_count = duckdb_row_count(result);
    const idx_t column_count = duckdb_column_count(result);

    LispVal rows = Nil;
    for (idx_t row = row_count; row > 0; --row) {
        LispVal row_value = Nil;
        const idx_t row_index = row - 1;
        for (idx_t column = column_count; column > 0; --column) {
            const idx_t column_index = column - 1;
            const char* name = duckdb_column_name(result, column_index);
            auto key = make_eta_symbol(
                runtime,
                (name != nullptr && name[0] != '\0') ? name : "column");
            if (!key) return std::unexpected(key.error());

            auto value = decode_result_value(runtime, result, column_index, row_index);
            if (!value) return std::unexpected(value.error());

            auto pair = make_eta_cons(runtime, *key, *value);
            if (!pair) return std::unexpected(pair.error());

            auto row_cell = make_eta_cons(runtime, *pair, row_value);
            if (!row_cell) return std::unexpected(row_cell.error());
            row_value = *row_cell;
        }

        auto row_entry = make_eta_cons(runtime, row_value, rows);
        if (!row_entry) return std::unexpected(row_entry.error());
        rows = *row_entry;
    }

    return rows;
}

[[nodiscard]] PrimitiveResult run_simple_query(
    RuntimeBinding& runtime,
    duckdb_connection connection,
    const std::string& sql) {
    duckdb_result result{};
    const duckdb_state status = duckdb_query(connection, sql.c_str(), &result);
    if (status == DuckDBError) {
        const std::string message = prefixed_error(
            "duckdb-query",
            result_error_message(&result, "query failed"));
        duckdb_destroy_result(&result);
        return std::unexpected(user_error(message));
    }

    auto rows = materialize_rows(runtime, &result);
    duckdb_destroy_result(&result);
    if (!rows) return std::unexpected(rows.error());
    return *rows;
}

[[nodiscard]] PrimitiveResult run_prepared_query(
    RuntimeBinding& runtime,
    duckdb_connection connection,
    const std::string& sql,
    const PrimitiveArgs args) {
    duckdb_prepared_statement statement = nullptr;
    if (duckdb_prepare(connection, sql.c_str(), &statement) == DuckDBError) {
        const std::string message = prefixed_error(
            "duckdb-query",
            prepare_error_message(statement, "failed to prepare query"));
        duckdb_destroy_prepare(&statement);
        return std::unexpected(user_error(message));
    }

    const idx_t expected_params = duckdb_nparams(statement);
    const idx_t provided_params = static_cast<idx_t>(args.size() - 2u);
    if (expected_params != provided_params) {
        duckdb_destroy_prepare(&statement);
        return std::unexpected(type_error(
            prefixed_error(
                "duckdb-query",
                "expected " + std::to_string(static_cast<unsigned long long>(expected_params))
                    + " parameters but received "
                    + std::to_string(static_cast<unsigned long long>(provided_params)))));
    }

    for (idx_t i = 0; i < provided_params; ++i) {
        auto bind_result = bind_query_parameter(runtime, statement, i + 1, args[2u + i]);
        if (!bind_result) {
            duckdb_destroy_prepare(&statement);
            return std::unexpected(bind_result.error());
        }
    }

    duckdb_result result{};
    const duckdb_state execute_status = duckdb_execute_prepared(statement, &result);
    if (execute_status == DuckDBError) {
        const std::string message = prefixed_error(
            "duckdb-query",
            result_error_message(&result, "failed to execute prepared query"));
        duckdb_destroy_result(&result);
        duckdb_destroy_prepare(&statement);
        return std::unexpected(user_error(message));
    }

    auto rows = materialize_rows(runtime, &result);
    duckdb_destroy_result(&result);
    duckdb_destroy_prepare(&statement);
    if (!rows) return std::unexpected(rows.error());
    return *rows;
}

PrimitiveResult primitive_open(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("duckdb-open: expected path argument"));
    }

    auto runtime = runtime_binding("duckdb-open");
    if (!runtime) return std::unexpected(runtime.error());
    auto path = require_string_arg(*runtime, args[0], "duckdb-open", "first argument");
    if (!path) return std::unexpected(path.error());

    duckdb_database database = nullptr;
    char* open_error = nullptr;
    const duckdb_state open_status =
        duckdb_open_ext(path->c_str(), &database, nullptr, &open_error);
    if (open_status == DuckDBError || database == nullptr) {
        const std::string message = prefixed_error("duckdb-open", take_open_error_message(open_error));
        const std::lock_guard<std::mutex> guard(g_state_mutex);
        set_last_error_locked(std::nullopt, message);
        return std::unexpected(user_error(message));
    }

    duckdb_connection connection = nullptr;
    if (duckdb_connect(database, &connection) == DuckDBError || connection == nullptr) {
        duckdb_close(&database);
        const std::string message = prefixed_error("duckdb-open", "failed to create DuckDB connection");
        const std::lock_guard<std::mutex> guard(g_state_mutex);
        set_last_error_locked(std::nullopt, message);
        return std::unexpected(user_error(message));
    }

    std::int64_t connection_id = 0;
    {
        const std::lock_guard<std::mutex> guard(g_state_mutex);
        connection_id = g_next_connection_id++;
        g_connections.emplace(
            connection_id,
            ConnectionState{
                .open = true,
                .last_error = {},
                .database = database,
                .connection = connection,
            });
        clear_last_error_locked(connection_id);
    }

    auto boxed = make_eta_fixnum(*runtime, connection_id);
    if (!boxed) {
        const std::lock_guard<std::mutex> guard(g_state_mutex);
        auto found = g_connections.find(connection_id);
        if (found != g_connections.end()) {
            close_connection_handles(found->second);
            g_connections.erase(found);
        }
        return std::unexpected(boxed.error());
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
    close_connection_handles(found->second);
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

    auto runtime = runtime_binding("duckdb-exec");
    if (!runtime) return std::unexpected(runtime.error());
    auto sql = require_string_arg(*runtime, args[1], "duckdb-exec", "second argument");
    if (!sql) return std::unexpected(sql.error());

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

    duckdb_result result{};
    const duckdb_state exec_status = duckdb_query(found->second.connection, sql->c_str(), &result);
    if (exec_status == DuckDBError) {
        const std::string message = prefixed_error(
            "duckdb-exec",
            result_error_message(&result, "statement execution failed"));
        duckdb_destroy_result(&result);
        set_last_error_locked(*connection_id, message);
        return std::unexpected(user_error(message));
    }
    duckdb_destroy_result(&result);

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

    auto runtime = runtime_binding("duckdb-query");
    if (!runtime) return std::unexpected(runtime.error());
    auto sql = require_string_arg(*runtime, args[1], "duckdb-query", "second argument");
    if (!sql) return std::unexpected(sql.error());

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

    PrimitiveResult result = std::unexpected(internal_error("duckdb-query: query execution failed"));
    if (args.size() == 2u) {
        result = run_simple_query(*runtime, found->second.connection, *sql);
    } else {
        result = run_prepared_query(*runtime, found->second.connection, *sql, args);
    }

    if (!result) {
        set_last_error_locked(*connection_id, runtime_error_message(result.error()));
        return std::unexpected(result.error());
    }

    clear_last_error_locked(*connection_id);
    return *result;
}

PrimitiveResult primitive_last_error(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error(
            "duckdb-last-error: expected connection handle or nil"));
    }

    auto runtime = runtime_binding("duckdb-last-error");
    if (!runtime) return std::unexpected(runtime.error());

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

    if (message.empty()) return False;
    return make_eta_string(*runtime, message);
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

    auto runtime = detail::runtime_binding_from_api(api, "duckdb sidecar");
    if (!runtime) {
        if (api->report_error != nullptr) {
            const std::string message = runtime_error_message(runtime.error());
            api->report_error(api->user_data, message.c_str());
        }
        return ETA_NATIVE_STATUS_ERROR;
    }

    {
        const std::lock_guard<std::mutex> guard(g_state_mutex);
        for (auto& [id, state] : g_connections) {
            (void)id;
            close_connection_handles(state);
        }
        g_connections.clear();
        g_last_error.clear();
        g_next_connection_id = 1;
    }
    g_runtime_binding = *runtime;

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
