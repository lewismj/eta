#pragma once

/**
 * @file os_primitives.h
 * @brief Builtin OS/filesystem primitives for environment variables,
 *        process arguments, process control, and filesystem metadata.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/error.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/vm/vm.h"

namespace eta::runtime {

using namespace eta::runtime::error;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::nanbox;

using Args = std::span<const LispVal>;
namespace fs = std::filesystem;

inline RuntimeError os_type_error(std::string message) {
    return RuntimeError{VMError{RuntimeErrorCode::TypeError, std::move(message)}};
}

inline std::expected<std::string, RuntimeError>
require_string_arg(InternTable& intern_table, LispVal value, const char* who, const char* arg_name = "argument") {
    auto sv = StringView::try_from(value, intern_table);
    if (!sv) {
        return std::unexpected(os_type_error(std::string(who) + ": " + arg_name + " must be a string"));
    }
    return std::string(sv->view());
}

inline std::expected<fs::path, RuntimeError>
require_path_arg(InternTable& intern_table, LispVal value, const char* who) {
    auto text = require_string_arg(intern_table, value, who, "path");
    if (!text) return std::unexpected(text.error());
    return fs::path(*text);
}

inline std::expected<LispVal, RuntimeError>
build_string_list(Heap& heap, InternTable& intern_table, const std::vector<std::string>& values) {
    LispVal list = Nil;
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        auto text = make_string(heap, intern_table, *it);
        if (!text) return std::unexpected(text.error());
        auto cell = make_cons(heap, *text, list);
        if (!cell) return std::unexpected(cell.error());
        list = *cell;
    }
    return list;
}

inline std::expected<LispVal, RuntimeError>
build_string_alist(
    Heap& heap,
    InternTable& intern_table,
    const std::vector<std::pair<std::string, std::string>>& entries)
{
    LispVal list = Nil;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        auto key = make_string(heap, intern_table, it->first);
        if (!key) return std::unexpected(key.error());
        auto value = make_string(heap, intern_table, it->second);
        if (!value) return std::unexpected(value.error());
        auto pair = make_cons(heap, *key, *value);
        if (!pair) return std::unexpected(pair.error());
        auto cell = make_cons(heap, *pair, list);
        if (!cell) return std::unexpected(cell.error());
        list = *cell;
    }
    return list;
}

inline std::expected<LispVal, RuntimeError>
bool_to_lisp(bool value) {
    return value ? True : False;
}

inline std::expected<std::int64_t, RuntimeError>
require_int64_arg(Heap& heap, LispVal value, const char* who, const char* arg_name = "argument") {
    auto n = classify_numeric(value, heap);
    if (!n.is_valid() || !n.is_fixnum()) {
        return std::unexpected(os_type_error(std::string(who) + ": " + arg_name + " must be an integer"));
    }
    return n.int_val;
}

inline std::expected<int, RuntimeError>
decode_exit_code(Heap& heap, LispVal value) {
    if (value == True) return 0;
    if (value == False) return 1;

    auto n = classify_numeric(value, heap);
    if (!n.is_valid() || !n.is_fixnum()) {
        return std::unexpected(os_type_error("exit: status must be #t, #f, or an integer"));
    }
    return static_cast<int>(n.int_val);
}

inline std::string temp_suffix() {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto mixed = now ^ (counter.fetch_add(1, std::memory_order_relaxed) + 0x9e3779b97f4a7c15ull);
    std::ostringstream oss;
    oss << std::hex << mixed;
    return oss.str();
}

inline std::expected<fs::path, RuntimeError> make_temp_base_dir() {
    std::error_code ec;
    auto dir = fs::temp_directory_path(ec);
    if (ec) {
        return std::unexpected(os_type_error("temp-directory-path: unable to resolve system temp directory"));
    }
    return dir;
}

inline std::expected<fs::path, RuntimeError> create_temp_file_path() {
    auto base = make_temp_base_dir();
    if (!base) return std::unexpected(base.error());

    for (int attempt = 0; attempt < 256; ++attempt) {
        const fs::path candidate = *base / ("eta-" + temp_suffix() + ".tmp");
        const std::string utf8_path = candidate.string();
#ifdef _WIN32
        int fd = _open(
            utf8_path.c_str(),
            _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
            _S_IREAD | _S_IWRITE);
        if (fd >= 0) {
            _close(fd);
            return candidate;
        }
#else
        int fd = ::open(utf8_path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0) {
            ::close(fd);
            return candidate;
        }
        if (errno != EEXIST) {
            return std::unexpected(os_type_error("temp-file: failed to create temporary file"));
        }
#endif
    }

    return std::unexpected(os_type_error("temp-file: unable to allocate unique temporary file"));
}

inline std::expected<fs::path, RuntimeError> create_temp_directory_path() {
    auto base = make_temp_base_dir();
    if (!base) return std::unexpected(base.error());

    for (int attempt = 0; attempt < 256; ++attempt) {
        const fs::path candidate = *base / ("eta-" + temp_suffix() + ".dir");
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) {
            return candidate;
        }
        if (ec && ec != std::errc::file_exists) {
            return std::unexpected(os_type_error("temp-directory: failed to create temporary directory"));
        }
    }

    return std::unexpected(os_type_error("temp-directory: unable to allocate unique temporary directory"));
}

inline std::int64_t file_time_to_epoch_ms(const fs::file_time_type file_tp) {
    using namespace std::chrono;
    const auto now_file = fs::file_time_type::clock::now();
    const auto now_sys = system_clock::now();
    const auto sys_tp = time_point_cast<milliseconds>(file_tp - now_file + now_sys);
    return static_cast<std::int64_t>(sys_tp.time_since_epoch().count());
}

inline std::expected<std::vector<std::pair<std::string, std::string>>, RuntimeError>
collect_environment_variables() {
    std::vector<std::pair<std::string, std::string>> out;

#ifdef _WIN32
    LPCH block = ::GetEnvironmentStringsA();
    if (block == nullptr) {
        return std::unexpected(os_type_error("environment-variables: failed to enumerate environment"));
    }
    for (LPCH cur = block; *cur != '\0';) {
        std::string entry(cur);
        cur += entry.size() + 1;
        if (entry.empty() || entry[0] == '=') continue;
        auto pos = entry.find('=');
        if (pos == std::string::npos) {
            out.emplace_back(entry, std::string{});
        } else {
            out.emplace_back(entry.substr(0, pos), entry.substr(pos + 1));
        }
    }
    ::FreeEnvironmentStringsA(block);
#else
    extern char** environ;
    char** envp = environ;
    if (!envp) {
        return out;
    }
    for (char** cur = envp; *cur != nullptr; ++cur) {
        std::string entry(*cur);
        if (entry.empty() || entry[0] == '=') continue;
        auto pos = entry.find('=');
        if (pos == std::string::npos) {
            out.emplace_back(entry, std::string{});
        } else {
            out.emplace_back(entry.substr(0, pos), entry.substr(pos + 1));
        }
    }
#endif

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.first == b.first) return a.second < b.second;
        return a.first < b.first;
    });
    return out;
}

inline void register_os_primitives(
    BuiltinEnvironment& env,
    Heap& heap,
    InternTable& intern_table,
    [[maybe_unused]] vm::VM& vm,
    std::span<const std::string> command_line_arguments = {})
{
    const std::vector<std::string> cli_args(command_line_arguments.begin(), command_line_arguments.end());

    env.register_builtin("getenv", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto key = require_string_arg(intern_table, args[0], "getenv", "name");
        if (!key) return std::unexpected(key.error());
        const char* value = std::getenv(key->c_str());
        if (value == nullptr) return False;
        return make_string(heap, intern_table, value);
    });

    env.register_builtin("setenv!", 2, false, [&intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto key = require_string_arg(intern_table, args[0], "setenv!", "name");
        if (!key) return std::unexpected(key.error());
        auto value = require_string_arg(intern_table, args[1], "setenv!", "value");
        if (!value) return std::unexpected(value.error());

#ifdef _WIN32
        if (_putenv_s(key->c_str(), value->c_str()) != 0) {
            return std::unexpected(os_type_error("setenv!: failed to set environment variable"));
        }
#else
        if (::setenv(key->c_str(), value->c_str(), 1) != 0) {
            return std::unexpected(os_type_error("setenv!: failed to set environment variable"));
        }
#endif
        return Nil;
    });

    env.register_builtin("unsetenv!", 1, false, [&intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto key = require_string_arg(intern_table, args[0], "unsetenv!", "name");
        if (!key) return std::unexpected(key.error());

#ifdef _WIN32
        if (_putenv_s(key->c_str(), "") != 0) {
            return std::unexpected(os_type_error("unsetenv!: failed to unset environment variable"));
        }
#else
        if (::unsetenv(key->c_str()) != 0) {
            return std::unexpected(os_type_error("unsetenv!: failed to unset environment variable"));
        }
#endif
        return Nil;
    });

    env.register_builtin("environment-variables", 0, false, [&heap, &intern_table](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        auto vars = collect_environment_variables();
        if (!vars) return std::unexpected(vars.error());
        return build_string_alist(heap, intern_table, *vars);
    });

    env.register_builtin("command-line-arguments", 0, false, [&heap, &intern_table, cli_args](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        return build_string_list(heap, intern_table, cli_args);
    });

    env.register_builtin("exit", 0, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        int code = 0;
        if (!args.empty()) {
            auto decoded = decode_exit_code(heap, args[0]);
            if (!decoded) return std::unexpected(decoded.error());
            code = *decoded;
        }
        std::exit(code);
    });

    env.register_builtin("current-directory", 0, false, [&heap, &intern_table](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        std::error_code ec;
        auto cwd = fs::current_path(ec);
        if (ec) return std::unexpected(os_type_error("current-directory: failed to query current directory"));
        return make_string(heap, intern_table, cwd.make_preferred().string());
    });

    env.register_builtin("change-directory!", 1, false, [&intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto path = require_path_arg(intern_table, args[0], "change-directory!");
        if (!path) return std::unexpected(path.error());
        std::error_code ec;
        fs::current_path(*path, ec);
        if (ec) return std::unexpected(os_type_error("change-directory!: failed to change current directory"));
        return Nil;
    });

    env.register_builtin("file-exists?", 1, false, [&intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto path = require_path_arg(intern_table, args[0], "file-exists?");
        if (!path) return std::unexpected(path.error());
        std::error_code ec;
        const bool exists = fs::exists(*path, ec);
        if (ec) return std::unexpected(os_type_error("file-exists?: filesystem query failed"));
        return bool_to_lisp(exists);
    });

    env.register_builtin("directory?", 1, false, [&intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto path = require_path_arg(intern_table, args[0], "directory?");
        if (!path) return std::unexpected(path.error());
        std::error_code ec;
        const bool is_dir = fs::is_directory(*path, ec);
        if (ec) return std::unexpected(os_type_error("directory?: filesystem query failed"));
        return bool_to_lisp(is_dir);
    });

    env.register_builtin("delete-file", 1, false, [&intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto path = require_path_arg(intern_table, args[0], "delete-file");
        if (!path) return std::unexpected(path.error());
        std::error_code ec;
        if (fs::is_directory(*path, ec)) {
            return std::unexpected(os_type_error("delete-file: path is a directory"));
        }
        if (ec) return std::unexpected(os_type_error("delete-file: filesystem query failed"));
        const bool removed = fs::remove(*path, ec);
        if (ec || !removed) {
            return std::unexpected(os_type_error("delete-file: failed to remove file"));
        }
        return Nil;
    });

    env.register_builtin("make-directory", 1, false, [&intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto path = require_path_arg(intern_table, args[0], "make-directory");
        if (!path) return std::unexpected(path.error());
        std::error_code ec;
        if (fs::create_directory(*path, ec)) return Nil;
        if (ec) return std::unexpected(os_type_error("make-directory: failed to create directory"));
        if (fs::is_directory(*path, ec) && !ec) return Nil;
        return std::unexpected(os_type_error("make-directory: target exists and is not a directory"));
    });

    env.register_builtin("list-directory", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto path = require_path_arg(intern_table, args[0], "list-directory");
        if (!path) return std::unexpected(path.error());
        std::error_code ec;
        if (!fs::is_directory(*path, ec) || ec) {
            return std::unexpected(os_type_error("list-directory: path is not a directory"));
        }

        std::vector<std::string> entries;
        for (const auto& entry : fs::directory_iterator(*path, ec)) {
            if (ec) {
                return std::unexpected(os_type_error("list-directory: failed while enumerating directory"));
            }
            entries.push_back(entry.path().filename().string());
        }
        std::sort(entries.begin(), entries.end());
        return build_string_list(heap, intern_table, entries);
    });

    env.register_builtin("path-join", 1, true, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto first = require_path_arg(intern_table, args[0], "path-join");
        if (!first) return std::unexpected(first.error());
        fs::path out = *first;
        for (std::size_t i = 1; i < args.size(); ++i) {
            auto segment = require_path_arg(intern_table, args[i], "path-join");
            if (!segment) return std::unexpected(segment.error());
            out /= *segment;
        }
        return make_string(heap, intern_table, out.make_preferred().string());
    });

    env.register_builtin("path-split", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto path = require_path_arg(intern_table, args[0], "path-split");
        if (!path) return std::unexpected(path.error());

        std::vector<std::string> parts;
        if (path->has_root_name() || path->has_root_directory()) {
            std::string root = path->root_name().string() + path->root_directory().string();
            if (!root.empty()) parts.push_back(std::move(root));
        }
        for (const auto& part : path->relative_path()) {
            auto text = part.string();
            if (!text.empty()) parts.push_back(std::move(text));
        }
        return build_string_list(heap, intern_table, parts);
    });

    env.register_builtin("path-normalize", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto path = require_path_arg(intern_table, args[0], "path-normalize");
        if (!path) return std::unexpected(path.error());
        auto normalized = path->lexically_normal().make_preferred().string();
        return make_string(heap, intern_table, normalized);
    });

    env.register_builtin("temp-file", 0, false, [&heap, &intern_table](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        auto path = create_temp_file_path();
        if (!path) return std::unexpected(path.error());
        return make_string(heap, intern_table, path->make_preferred().string());
    });

    env.register_builtin("temp-directory", 0, false, [&heap, &intern_table](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        auto path = create_temp_directory_path();
        if (!path) return std::unexpected(path.error());
        return make_string(heap, intern_table, path->make_preferred().string());
    });

    env.register_builtin("file-modification-time", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto path = require_path_arg(intern_table, args[0], "file-modification-time");
        if (!path) return std::unexpected(path.error());
        std::error_code ec;
        auto stamp = fs::last_write_time(*path, ec);
        if (ec) return std::unexpected(os_type_error("file-modification-time: failed to query modification time"));
        return make_fixnum(heap, file_time_to_epoch_ms(stamp));
    });

    env.register_builtin("file-size", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto path = require_path_arg(intern_table, args[0], "file-size");
        if (!path) return std::unexpected(path.error());
        std::error_code ec;
        const auto size = fs::file_size(*path, ec);
        if (ec) return std::unexpected(os_type_error("file-size: failed to query file size"));
        return make_fixnum(heap, static_cast<std::int64_t>(size));
    });
}

} ///< namespace eta::runtime
