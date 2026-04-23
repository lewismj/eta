#pragma once

/**
 * @file time_primitives.h
 * @brief Builtin time primitives for wall-clock time, monotonic timing,
 *        sleep, UTC/local calendar breakdown, and ISO-8601 formatting.
 */

#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <expected>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/error.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/vm/vm.h"

namespace eta::runtime {

using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::error;

using Args = const std::vector<LispVal>&;

inline RuntimeError time_type_error(const std::string& message) {
    return RuntimeError{VMError{RuntimeErrorCode::TypeError, message}};
}

/**
 * Convert epoch milliseconds to whole epoch seconds using floor division.
 * This keeps negative timestamps correct (e.g. -1 ms -> -1 second).
 */
inline std::int64_t floor_div_millis_to_seconds(std::int64_t epoch_ms) {
    std::int64_t q = epoch_ms / 1000;
    const std::int64_t r = epoch_ms % 1000;
    if (r < 0) --q;
    return q;
}

inline std::expected<std::int64_t, RuntimeError> require_integer_arg(
    Heap& heap, LispVal value, const char* who, bool require_non_negative = false)
{
    auto n = classify_numeric(value, heap);
    if (!n.is_valid()) {
        if (require_non_negative) {
            return std::unexpected(time_type_error(std::string(who) + ": argument must be a non-negative integer"));
        }
        return std::unexpected(time_type_error(std::string(who) + ": argument must be an integer epoch-ms"));
    }

    std::int64_t out = 0;
    if (n.is_fixnum()) {
        out = n.int_val;
    } else {
        const double d = n.float_val;
        if (!std::isfinite(d) || std::floor(d) != d ||
            d < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
            d > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            if (require_non_negative) {
                return std::unexpected(time_type_error(std::string(who) + ": argument must be a non-negative integer"));
            }
            return std::unexpected(time_type_error(std::string(who) + ": argument must be an integer epoch-ms"));
        }
        out = static_cast<std::int64_t>(d);
    }

    if (require_non_negative && out < 0) {
        return std::unexpected(time_type_error(std::string(who) + ": argument must be a non-negative integer"));
    }
    return out;
}

inline std::expected<std::time_t, RuntimeError>
epoch_ms_to_time_t(std::int64_t epoch_ms, const char* who) {
    const std::int64_t epoch_seconds = floor_div_millis_to_seconds(epoch_ms);

    if constexpr (std::numeric_limits<std::time_t>::is_signed) {
        if (epoch_seconds < static_cast<std::int64_t>(std::numeric_limits<std::time_t>::min()) ||
            epoch_seconds > static_cast<std::int64_t>(std::numeric_limits<std::time_t>::max())) {
            return std::unexpected(time_type_error(std::string(who) + ": epoch-ms out of supported range"));
        }
        return static_cast<std::time_t>(epoch_seconds);
    }

    if (epoch_seconds < 0 ||
        static_cast<std::uint64_t>(epoch_seconds) > std::numeric_limits<std::time_t>::max()) {
        return std::unexpected(time_type_error(std::string(who) + ": epoch-ms out of supported range"));
    }
    return static_cast<std::time_t>(epoch_seconds);
}

inline bool safe_gmtime(const std::time_t& time, std::tm& out_tm) {
#ifdef _WIN32
    return ::gmtime_s(&out_tm, &time) == 0;
#else
    return ::gmtime_r(&time, &out_tm) != nullptr;
#endif
}

inline bool safe_localtime(const std::time_t& time, std::tm& out_tm) {
#ifdef _WIN32
    return ::localtime_s(&out_tm, &time) == 0;
#else
    return ::localtime_r(&time, &out_tm) != nullptr;
#endif
}

inline std::expected<std::int32_t, RuntimeError>
local_utc_offset_minutes(std::time_t epoch_seconds, const char* who) {
    std::tm local_tm{};
    std::tm utc_tm{};
    if (!safe_localtime(epoch_seconds, local_tm) || !safe_gmtime(epoch_seconds, utc_tm)) {
        return std::unexpected(time_type_error(std::string(who) + ": failed to compute timezone offset"));
    }

    /**
     * Compute offset by interpreting the UTC tm as local and comparing to the
     * actual local tm conversion.
     */
    std::tm local_copy = local_tm;
    std::tm utc_as_local = utc_tm;

    const std::time_t local_epoch = std::mktime(&local_copy);
    const std::time_t utc_epoch_as_local = std::mktime(&utc_as_local);
    if (local_epoch == static_cast<std::time_t>(-1) ||
        utc_epoch_as_local == static_cast<std::time_t>(-1)) {
        return std::unexpected(time_type_error(std::string(who) + ": failed to compute timezone offset"));
    }

    const double delta_seconds = std::difftime(local_epoch, utc_epoch_as_local);
    if (!std::isfinite(delta_seconds)) {
        return std::unexpected(time_type_error(std::string(who) + ": failed to compute timezone offset"));
    }
    return static_cast<std::int32_t>(delta_seconds / 60.0);
}

inline std::string format_iso_date_time(const std::tm& tm_parts) {
    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << (tm_parts.tm_year + 1900) << '-'
        << std::setw(2) << (tm_parts.tm_mon + 1) << '-'
        << std::setw(2) << tm_parts.tm_mday << 'T'
        << std::setw(2) << tm_parts.tm_hour << ':'
        << std::setw(2) << tm_parts.tm_min << ':'
        << std::setw(2) << tm_parts.tm_sec;
    return oss.str();
}

inline std::expected<LispVal, RuntimeError>
make_alist_pair(Heap& heap, InternTable& intern_table, const char* key, LispVal value) {
    auto sym = make_symbol(intern_table, key);
    if (!sym) return std::unexpected(sym.error());
    return make_cons(heap, *sym, value);
}

inline std::expected<void, RuntimeError>
prepend_alist_pair(
    Heap& heap, InternTable& intern_table, const char* key, LispVal value, LispVal& list_out)
{
    auto pair = make_alist_pair(heap, intern_table, key, value);
    if (!pair) return std::unexpected(pair.error());
    auto cell = make_cons(heap, *pair, list_out);
    if (!cell) return std::unexpected(cell.error());
    list_out = *cell;
    return {};
}

inline std::expected<void, RuntimeError>
prepend_alist_fixnum(
    Heap& heap,
    InternTable& intern_table,
    const char* key,
    std::int64_t value,
    LispVal& list_out)
{
    auto num = make_fixnum(heap, value);
    if (!num) return std::unexpected(num.error());
    return prepend_alist_pair(heap, intern_table, key, *num, list_out);
}

inline std::expected<LispVal, RuntimeError>
build_parts_alist(
    Heap& heap,
    InternTable& intern_table,
    const std::tm& tm_parts,
    std::int32_t offset_minutes)
{
    LispVal result = Nil;

    auto add_fix = [&](const char* key, std::int64_t value) -> std::expected<void, RuntimeError> {
        return prepend_alist_fixnum(heap, intern_table, key, value, result);
    };

    if (auto r = add_fix("offset-minutes", offset_minutes); !r) return std::unexpected(r.error());
    if (auto r = prepend_alist_pair(heap, intern_table, "is-dst", tm_parts.tm_isdst > 0 ? True : False, result); !r)
        return std::unexpected(r.error());
    if (auto r = add_fix("yearday", tm_parts.tm_yday); !r) return std::unexpected(r.error());
    if (auto r = add_fix("weekday", tm_parts.tm_wday); !r) return std::unexpected(r.error());
    if (auto r = add_fix("second", tm_parts.tm_sec); !r) return std::unexpected(r.error());
    if (auto r = add_fix("minute", tm_parts.tm_min); !r) return std::unexpected(r.error());
    if (auto r = add_fix("hour", tm_parts.tm_hour); !r) return std::unexpected(r.error());
    if (auto r = add_fix("day", tm_parts.tm_mday); !r) return std::unexpected(r.error());
    if (auto r = add_fix("month", tm_parts.tm_mon + 1); !r) return std::unexpected(r.error());
    if (auto r = add_fix("year", tm_parts.tm_year + 1900); !r) return std::unexpected(r.error());

    return result;
}

inline std::expected<LispVal, RuntimeError> make_parts(
    Heap& heap,
    InternTable& intern_table,
    std::int64_t epoch_ms,
    bool local,
    const char* who)
{
    auto epoch_seconds = epoch_ms_to_time_t(epoch_ms, who);
    if (!epoch_seconds) return std::unexpected(epoch_seconds.error());

    std::tm tm_parts{};
    if (local) {
        if (!safe_localtime(*epoch_seconds, tm_parts)) {
            return std::unexpected(time_type_error(std::string(who) + ": local time conversion failed"));
        }
    } else {
        if (!safe_gmtime(*epoch_seconds, tm_parts)) {
            return std::unexpected(time_type_error(std::string(who) + ": utc time conversion failed"));
        }
    }

    std::int32_t offset_minutes = 0;
    if (local) {
        auto offset = local_utc_offset_minutes(*epoch_seconds, who);
        if (!offset) return std::unexpected(offset.error());
        offset_minutes = *offset;
    }

    return build_parts_alist(heap, intern_table, tm_parts, offset_minutes);
}

inline void register_time_primitives(
    BuiltinEnvironment& env,
    Heap& heap,
    InternTable& intern_table,
    [[maybe_unused]] vm::VM* vm = nullptr)
{
    env.register_builtin("%time-now-ms", 0, false, [&heap](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto count = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        return make_fixnum(heap, static_cast<std::int64_t>(count));
    });

    env.register_builtin("%time-now-us", 0, false, [&heap](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto count = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
        return make_fixnum(heap, static_cast<std::int64_t>(count));
    });

    env.register_builtin("%time-now-ns", 0, false, [&heap](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        return make_fixnum(heap, static_cast<std::int64_t>(count));
    });

    env.register_builtin("%time-monotonic-ms", 0, false, [&heap](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto count = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        return make_fixnum(heap, static_cast<std::int64_t>(count));
    });

    env.register_builtin("%time-sleep-ms", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ms = require_integer_arg(heap, args[0], "%time-sleep-ms", true);
        if (!ms) return std::unexpected(ms.error());
        std::this_thread::sleep_for(std::chrono::milliseconds(*ms));
        return Nil;
    });

    env.register_builtin("%time-utc-parts", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto epoch_ms = require_integer_arg(heap, args[0], "%time-utc-parts");
        if (!epoch_ms) return std::unexpected(epoch_ms.error());
        return make_parts(heap, intern_table, *epoch_ms, false, "%time-utc-parts");
    });

    env.register_builtin("%time-local-parts", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto epoch_ms = require_integer_arg(heap, args[0], "%time-local-parts");
        if (!epoch_ms) return std::unexpected(epoch_ms.error());
        return make_parts(heap, intern_table, *epoch_ms, true, "%time-local-parts");
    });

    env.register_builtin("%time-format-iso8601-utc", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto epoch_ms = require_integer_arg(heap, args[0], "%time-format-iso8601-utc");
        if (!epoch_ms) return std::unexpected(epoch_ms.error());

        auto epoch_seconds = epoch_ms_to_time_t(*epoch_ms, "%time-format-iso8601-utc");
        if (!epoch_seconds) return std::unexpected(epoch_seconds.error());

        std::tm tm_parts{};
        if (!safe_gmtime(*epoch_seconds, tm_parts)) {
            return std::unexpected(time_type_error("%time-format-iso8601-utc: utc time conversion failed"));
        }
        return make_string(heap, intern_table, format_iso_date_time(tm_parts) + "Z");
    });

    env.register_builtin("%time-format-iso8601-local", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto epoch_ms = require_integer_arg(heap, args[0], "%time-format-iso8601-local");
        if (!epoch_ms) return std::unexpected(epoch_ms.error());

        auto epoch_seconds = epoch_ms_to_time_t(*epoch_ms, "%time-format-iso8601-local");
        if (!epoch_seconds) return std::unexpected(epoch_seconds.error());

        std::tm tm_parts{};
        if (!safe_localtime(*epoch_seconds, tm_parts)) {
            return std::unexpected(time_type_error("%time-format-iso8601-local: local time conversion failed"));
        }
        auto offset = local_utc_offset_minutes(*epoch_seconds, "%time-format-iso8601-local");
        if (!offset) return std::unexpected(offset.error());

        const auto minutes = *offset;
        const char sign = (minutes >= 0) ? '+' : '-';
        const auto abs_minutes = (minutes >= 0) ? minutes : -minutes;
        const auto hours_part = abs_minutes / 60;
        const auto mins_part = abs_minutes % 60;

        std::ostringstream oss;
        oss << format_iso_date_time(tm_parts)
            << sign
            << std::setfill('0')
            << std::setw(2) << hours_part
            << ':'
            << std::setw(2) << mins_part;
        return make_string(heap, intern_table, oss.str());
    });
}

} ///< namespace eta::runtime
