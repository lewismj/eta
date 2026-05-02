#include "eta/runtime/process_primitives.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <cwctype>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "eta/runtime/error.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/port.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/types/types.h"

namespace eta::runtime {
using namespace eta::runtime::error;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::nanbox;

namespace {
using Args = std::span<const LispVal>;

inline constexpr const char* kSpawnFailedPrefix = "process-spawn-failed";
inline constexpr const char* kNotFoundPrefix = "process-not-found";
inline constexpr const char* kTimeoutPrefix = "process-timeout";
inline constexpr const char* kWaitFailedPrefix = "process-wait-failed";

enum class StdinMode : std::uint8_t {
    Pipe,
    Inherit,
    Null,
    Data,
};

enum class StreamMode : std::uint8_t {
    Pipe,
    Capture,
    Inherit,
    Null,
    Merge,
};

struct ProcessOptions {
    std::optional<std::string> cwd;
    bool use_environment{false};
    bool replace_env{false};
    std::vector<std::pair<std::string, std::string>> env;
    StdinMode stdin_mode{StdinMode::Pipe};
    StreamMode stdout_mode{StreamMode::Pipe};
    StreamMode stderr_mode{StreamMode::Pipe};
    std::optional<std::int64_t> timeout_ms;
    bool binary{false};
    std::vector<std::uint8_t> stdin_data;
};

struct ParseDefaults {
    StdinMode stdin_mode{StdinMode::Pipe};
    StreamMode stdout_mode{StreamMode::Pipe};
    StreamMode stderr_mode{StreamMode::Pipe};
};

std::unexpected<RuntimeError> type_error(std::string message) {
    return std::unexpected(RuntimeError{
        VMError{RuntimeErrorCode::TypeError, std::move(message)}
    });
}

std::unexpected<RuntimeError> invalid_arity(std::string message) {
    return std::unexpected(RuntimeError{
        VMError{RuntimeErrorCode::InvalidArity, std::move(message)}
    });
}

std::unexpected<RuntimeError> prefixed_error(const char* prefix, std::string detail) {
    return std::unexpected(RuntimeError{
        VMError{RuntimeErrorCode::InternalError, std::string(prefix) + ": " + std::move(detail)}
    });
}

std::expected<std::string, RuntimeError> require_string_arg(
    InternTable& intern_table,
    LispVal value,
    const char* who,
    const char* label) {
    auto sv = StringView::try_from(value, intern_table);
    if (!sv) {
        return type_error(std::string(who) + ": " + label + " must be a string");
    }
    return std::string(sv->view());
}

std::expected<std::int64_t, RuntimeError> require_non_negative_int(
    Heap& heap,
    LispVal value,
    const char* who,
    const char* label) {
    auto n = classify_numeric(value, heap);
    if (!n.is_valid() || n.is_flonum() || n.int_val < 0) {
        return type_error(std::string(who) + ": " + label + " must be a non-negative integer");
    }
    return n.int_val;
}

std::expected<bool, RuntimeError> require_bool(
    LispVal value,
    const char* who,
    const char* label) {
    if (value == True) return true;
    if (value == False) return false;
    return type_error(std::string(who) + ": " + label + " must be #t or #f");
}

std::expected<std::string, RuntimeError> symbol_name(
    InternTable& intern_table,
    LispVal value,
    const char* who,
    const char* label) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::Symbol) {
        return type_error(std::string(who) + ": " + label + " must be a symbol");
    }
    auto text = intern_table.get_string(ops::payload(value));
    if (!text) {
        return std::unexpected(RuntimeError{
            VMError{RuntimeErrorCode::InternalError, std::string(who) + ": unresolved symbol id"}});
    }
    return std::string(*text);
}

std::expected<std::string, RuntimeError> symbol_or_string_name(
    InternTable& intern_table,
    LispVal value,
    const char* who,
    const char* label) {
    if (auto sv = StringView::try_from(value, intern_table)) {
        return std::string(sv->view());
    }
    auto sym = symbol_name(intern_table, value, who, label);
    if (!sym) return std::unexpected(sym.error());
    return *sym;
}

std::string normalize_key(std::string key) {
    if (!key.empty() && key.front() == ':') {
        key.erase(key.begin());
    }
    return key;
}

std::expected<std::vector<std::string>, RuntimeError> require_string_list(
    Heap& heap,
    InternTable& intern_table,
    LispVal list,
    const char* who,
    const char* label) {
    std::vector<std::string> out;
    LispVal cur = list;
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
            return type_error(std::string(who) + ": " + label + " must be a proper list of strings");
        }
        auto* cell = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!cell) {
            return type_error(std::string(who) + ": " + label + " must be a proper list of strings");
        }
        auto s = require_string_arg(intern_table, cell->car, who, label);
        if (!s) return std::unexpected(s.error());
        out.push_back(*s);
        cur = cell->cdr;
    }
    return out;
}

std::expected<std::vector<std::pair<std::string, LispVal>>, RuntimeError> extract_option_pairs(
    Heap& heap,
    InternTable& intern_table,
    LispVal options,
    const char* who) {
    std::vector<std::pair<std::string, LispVal>> out;
    if (options == Nil) return out;

    if (!ops::is_boxed(options) || ops::tag(options) != Tag::HeapObject) {
        return type_error(std::string(who) + ": options must be an alist, hash-map, or '()");
    }

    const auto id = ops::payload(options);
    if (auto* map = heap.try_get_as<ObjectKind::HashMap, types::HashMap>(id)) {
        for (std::size_t i = 0; i < map->state.size(); ++i) {
            if (map->state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
            auto key = symbol_or_string_name(intern_table, map->keys[i], who, "option key");
            if (!key) return std::unexpected(key.error());
            out.emplace_back(normalize_key(*key), map->values[i]);
        }
        return out;
    }

    LispVal cur = options;
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
            return type_error(std::string(who) + ": options must be a proper alist");
        }
        auto* cell = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!cell) return type_error(std::string(who) + ": options must be a proper alist");

        if (!ops::is_boxed(cell->car) || ops::tag(cell->car) != Tag::HeapObject) {
            return type_error(std::string(who) + ": option entry must be a pair");
        }
        auto* pair = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cell->car));
        if (!pair) return type_error(std::string(who) + ": option entry must be a pair");

        auto key = symbol_or_string_name(intern_table, pair->car, who, "option key");
        if (!key) return std::unexpected(key.error());
        out.emplace_back(normalize_key(*key), pair->cdr);
        cur = cell->cdr;
    }

    return out;
}

std::expected<std::vector<std::pair<std::string, std::string>>, RuntimeError> parse_environment_value(
    Heap& heap,
    InternTable& intern_table,
    LispVal value,
    const char* who) {
    std::vector<std::pair<std::string, std::string>> out;
    if (value == Nil) return out;

    auto pairs = extract_option_pairs(heap, intern_table, value, who);
    if (!pairs) return std::unexpected(pairs.error());

    out.reserve(pairs->size());
    for (const auto& [key, val] : *pairs) {
        auto s = require_string_arg(intern_table, val, who, "environment value");
        if (!s) return std::unexpected(s.error());
        out.emplace_back(key, *s);
    }
    return out;
}

std::expected<ProcessOptions, RuntimeError> parse_process_options(
    Heap& heap,
    InternTable& intern_table,
    LispVal options,
    const char* who,
    ParseDefaults defaults) {
    ProcessOptions parsed;
    parsed.stdin_mode = defaults.stdin_mode;
    parsed.stdout_mode = defaults.stdout_mode;
    parsed.stderr_mode = defaults.stderr_mode;

    auto pairs = extract_option_pairs(heap, intern_table, options, who);
    if (!pairs) return std::unexpected(pairs.error());

    for (const auto& [key, value] : *pairs) {
        if (key == "cwd") {
            auto cwd = require_string_arg(intern_table, value, who, "cwd");
            if (!cwd) return std::unexpected(cwd.error());
            parsed.cwd = std::move(*cwd);
            continue;
        }

        if (key == "env") {
            auto env = parse_environment_value(heap, intern_table, value, who);
            if (!env) return std::unexpected(env.error());
            parsed.use_environment = true;
            parsed.env = std::move(*env);
            continue;
        }

        if (key == "replace-env?") {
            auto replace_env = require_bool(value, who, "replace-env?");
            if (!replace_env) return std::unexpected(replace_env.error());
            parsed.replace_env = *replace_env;
            if (*replace_env) parsed.use_environment = true;
            continue;
        }

        if (key == "stdin") {
            if (auto sv = StringView::try_from(value, intern_table)) {
                const auto text = std::string(sv->view());
                parsed.stdin_mode = StdinMode::Data;
                parsed.stdin_data.assign(text.begin(), text.end());
                continue;
            }

            if (ops::is_boxed(value) && ops::tag(value) == Tag::HeapObject) {
                auto* bv = heap.try_get_as<ObjectKind::ByteVector, types::ByteVector>(ops::payload(value));
                if (bv) {
                    parsed.stdin_mode = StdinMode::Data;
                    parsed.stdin_data = bv->data;
                    continue;
                }
            }

            auto mode = symbol_or_string_name(intern_table, value, who, "stdin");
            if (!mode) return std::unexpected(mode.error());
            std::string normalized = normalize_key(*mode);
            if (normalized == "pipe") {
                parsed.stdin_mode = StdinMode::Pipe;
            } else if (normalized == "inherit") {
                parsed.stdin_mode = StdinMode::Inherit;
            } else if (normalized == "null") {
                parsed.stdin_mode = StdinMode::Null;
            } else {
                return type_error(std::string(who) + ": stdin must be string, bytevector, 'pipe, 'inherit, or 'null");
            }
            continue;
        }

        if (key == "stdout") {
            auto mode = symbol_or_string_name(intern_table, value, who, "stdout");
            if (!mode) return std::unexpected(mode.error());
            std::string normalized = normalize_key(*mode);
            if (normalized == "pipe") {
                parsed.stdout_mode = StreamMode::Pipe;
            } else if (normalized == "capture") {
                parsed.stdout_mode = StreamMode::Capture;
            } else if (normalized == "inherit") {
                parsed.stdout_mode = StreamMode::Inherit;
            } else if (normalized == "null") {
                parsed.stdout_mode = StreamMode::Null;
            } else {
                return type_error(std::string(who) + ": stdout must be 'pipe, 'capture, 'inherit, or 'null");
            }
            continue;
        }

        if (key == "stderr") {
            auto mode = symbol_or_string_name(intern_table, value, who, "stderr");
            if (!mode) return std::unexpected(mode.error());
            std::string normalized = normalize_key(*mode);
            if (normalized == "pipe") {
                parsed.stderr_mode = StreamMode::Pipe;
            } else if (normalized == "capture") {
                parsed.stderr_mode = StreamMode::Capture;
            } else if (normalized == "inherit") {
                parsed.stderr_mode = StreamMode::Inherit;
            } else if (normalized == "null") {
                parsed.stderr_mode = StreamMode::Null;
            } else if (normalized == "merge") {
                parsed.stderr_mode = StreamMode::Merge;
            } else {
                return type_error(std::string(who) + ": stderr must be 'pipe, 'capture, 'inherit, 'null, or 'merge");
            }
            continue;
        }

        if (key == "timeout-ms") {
            auto timeout_ms = require_non_negative_int(heap, value, who, "timeout-ms");
            if (!timeout_ms) return std::unexpected(timeout_ms.error());
            parsed.timeout_ms = *timeout_ms;
            continue;
        }

        if (key == "binary?") {
            auto binary = require_bool(value, who, "binary?");
            if (!binary) return std::unexpected(binary.error());
            parsed.binary = *binary;
            continue;
        }

        return type_error(std::string(who) + ": unknown option key '" + key + "'");
    }

    if (parsed.stderr_mode == StreamMode::Merge &&
        (parsed.stdout_mode == StreamMode::Null || parsed.stdout_mode == StreamMode::Inherit)) {
        /// Valid: stderr can still merge into inherited/null stdout stream.
    }

    return parsed;
}

#ifdef _WIN32
using NativePipeHandle = HANDLE;
constexpr NativePipeHandle kInvalidPipeHandle = nullptr;

bool is_valid_pipe_handle(const NativePipeHandle handle) {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

void close_pipe_handle(NativePipeHandle& handle) {
    if (is_valid_pipe_handle(handle)) {
        ::CloseHandle(handle);
    }
    handle = nullptr;
}

std::wstring utf8_to_utf16(const std::string& input) {
    if (input.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                           static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                          static_cast<int>(input.size()), out.data(), size);
    return out;
}

std::string win32_error_message(const DWORD err) {
    LPSTR buffer = nullptr;
    const DWORD size = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr);
    std::string out;
    if (size > 0 && buffer != nullptr) {
        out.assign(buffer, buffer + size);
        while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
    } else {
        out = "error " + std::to_string(static_cast<unsigned long long>(err));
    }
    if (buffer) ::LocalFree(buffer);
    return out;
}

std::wstring quote_windows_arg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";

    bool needs_quotes = false;
    for (const wchar_t ch : arg) {
        if (ch == L' ' || ch == L'\t' || ch == L'"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) return arg;

    std::wstring out;
    out.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        if (backslashes > 0) out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(ch);
    }
    if (backslashes > 0) out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring build_command_line(const std::string& program, const std::vector<std::string>& args) {
    std::wstring cmd = quote_windows_arg(utf8_to_utf16(program));
    for (const auto& arg : args) {
        cmd.push_back(L' ');
        cmd += quote_windows_arg(utf8_to_utf16(arg));
    }
    return cmd;
}

std::wstring normalize_env_key(std::wstring key) {
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towupper(static_cast<wint_t>(c)));
    });
    return key;
}

std::expected<std::vector<wchar_t>, RuntimeError> build_environment_block(const ProcessOptions& options) {
    std::unordered_map<std::wstring, std::wstring> env_map;
    if (!options.replace_env) {
        LPWCH block = ::GetEnvironmentStringsW();
        if (block == nullptr) {
            return prefixed_error(kSpawnFailedPrefix, "failed to read environment block");
        }
        for (LPWCH cur = block; *cur != L'\0';) {
            std::wstring entry(cur);
            cur += entry.size() + 1;
            if (entry.empty() || entry[0] == L'=') continue;
            const auto pos = entry.find(L'=');
            if (pos == std::wstring::npos || pos == 0) continue;
            const auto key = normalize_env_key(entry.substr(0, pos));
            env_map[key] = entry.substr(pos + 1);
        }
        ::FreeEnvironmentStringsW(block);
    }

    for (const auto& [key_utf8, value_utf8] : options.env) {
        const auto key = normalize_env_key(utf8_to_utf16(key_utf8));
        const auto value = utf8_to_utf16(value_utf8);
        env_map[key] = value;
    }

    std::vector<std::pair<std::wstring, std::wstring>> entries;
    entries.reserve(env_map.size());
    for (const auto& [k, v] : env_map) entries.emplace_back(k, v);
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    std::vector<wchar_t> block;
    for (const auto& [k, v] : entries) {
        block.insert(block.end(), k.begin(), k.end());
        block.push_back(L'=');
        block.insert(block.end(), v.begin(), v.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (block.size() == 1) block.push_back(L'\0');
    return block;
}
#else
using NativePipeHandle = int;
constexpr NativePipeHandle kInvalidPipeHandle = -1;

bool is_valid_pipe_handle(const NativePipeHandle handle) {
    return handle >= 0;
}

void close_pipe_handle(NativePipeHandle& handle) {
    if (is_valid_pipe_handle(handle)) {
        ::close(handle);
    }
    handle = -1;
}

std::vector<std::string> build_posix_environment(const ProcessOptions& options) {
    if (!options.use_environment) return {};

    std::unordered_map<std::string, std::string> env_map;
    if (!options.replace_env) {
        for (char** cur = environ; cur && *cur; ++cur) {
            std::string entry(*cur);
            const auto pos = entry.find('=');
            if (pos == std::string::npos || pos == 0) continue;
            env_map[entry.substr(0, pos)] = entry.substr(pos + 1);
        }
    }
    for (const auto& [k, v] : options.env) {
        env_map[k] = v;
    }

    std::vector<std::string> out;
    out.reserve(env_map.size());
    for (const auto& [k, v] : env_map) {
        out.push_back(k + "=" + v);
    }
    return out;
}

std::string lookup_env_path(const std::vector<std::string>& env) {
    for (const auto& entry : env) {
        if (entry.rfind("PATH=", 0) == 0) {
            return entry.substr(5);
        }
    }
    return {};
}
#endif

class ScopedPipeHandle {
public:
    ScopedPipeHandle() = default;
    explicit ScopedPipeHandle(const NativePipeHandle handle) : handle_(handle) {}

    ScopedPipeHandle(const ScopedPipeHandle&) = delete;
    ScopedPipeHandle& operator=(const ScopedPipeHandle&) = delete;

    ScopedPipeHandle(ScopedPipeHandle&& other) noexcept : handle_(other.release()) {}
    ScopedPipeHandle& operator=(ScopedPipeHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    ~ScopedPipeHandle() {
        reset(kInvalidPipeHandle);
    }

    void reset(const NativePipeHandle handle = kInvalidPipeHandle) {
        close_pipe_handle(handle_);
        handle_ = handle;
    }

    [[nodiscard]] NativePipeHandle get() const {
        return handle_;
    }

    [[nodiscard]] bool valid() const {
        return is_valid_pipe_handle(handle_);
    }

    NativePipeHandle release() {
        const auto value = handle_;
        handle_ = kInvalidPipeHandle;
        return value;
    }

private:
    NativePipeHandle handle_{kInvalidPipeHandle};
};

class NativePipePort final : public BytePort {
public:
    enum class Direction {
        Input,
        Output,
    };

    NativePipePort(NativePipeHandle handle, Direction direction)
        : handle_(handle), direction_(direction), open_(is_valid_pipe_handle(handle)) {}

    ~NativePipePort() override {
        (void)close();
    }

    std::optional<char32_t> read_char() override {
        if (direction_ != Direction::Input || !open_) return std::nullopt;
        auto first = read_byte();
        if (!first.has_value()) return std::nullopt;

        const std::uint8_t lead = *first;
        if ((lead & 0x80) == 0) {
            return static_cast<char32_t>(lead);
        }

        int continuation = 0;
        if ((lead & 0xE0) == 0xC0) continuation = 1;
        else if ((lead & 0xF0) == 0xE0) continuation = 2;
        else if ((lead & 0xF8) == 0xF0) continuation = 3;
        else return U'\uFFFD';

        std::string bytes;
        bytes.push_back(static_cast<char>(lead));
        for (int i = 0; i < continuation; ++i) {
            auto next = read_byte();
            if (!next.has_value()) return U'\uFFFD';
            bytes.push_back(static_cast<char>(*next));
        }

        std::size_t pos = 0;
        auto decoded = utf8::decode(bytes, pos);
        return decoded.has_value() ? decoded : std::optional<char32_t>(U'\uFFFD');
    }

    std::expected<void, RuntimeError> write_string(const std::string& str) override {
        if (direction_ != Direction::Output || !open_) {
            return std::unexpected(RuntimeError{
                VMError{RuntimeErrorCode::TypeError, "process port is not writable"}});
        }
        return write_raw(reinterpret_cast<const std::uint8_t*>(str.data()), str.size());
    }

    std::optional<std::uint8_t> read_byte() override {
        if (direction_ != Direction::Input || !open_) return std::nullopt;
#ifdef _WIN32
        std::uint8_t byte = 0;
        DWORD read = 0;
        if (!::ReadFile(handle_, &byte, 1, &read, nullptr) || read == 0) {
            return std::nullopt;
        }
        return byte;
#else
        std::uint8_t byte = 0;
        while (true) {
            const ssize_t n = ::read(handle_, &byte, 1);
            if (n == 1) return byte;
            if (n == 0) return std::nullopt;
            if (errno == EINTR) continue;
            return std::nullopt;
        }
#endif
    }

    std::expected<void, RuntimeError> write_byte(std::uint8_t byte) override {
        return write_raw(&byte, 1);
    }

    std::expected<void, RuntimeError> write_bytes(const std::vector<std::uint8_t>& bytes) override {
        if (bytes.empty()) return {};
        return write_raw(bytes.data(), bytes.size());
    }

    std::expected<void, RuntimeError> close() override {
        if (!open_) return {};
        close_pipe_handle(handle_);
        open_ = false;
        return {};
    }

    bool is_open() const override {
        return open_;
    }

    bool is_input() const override {
        return direction_ == Direction::Input;
    }

    bool is_output() const override {
        return direction_ == Direction::Output;
    }

private:
    std::expected<void, RuntimeError> write_raw(const std::uint8_t* data, std::size_t size) {
        if (direction_ != Direction::Output || !open_) {
            return std::unexpected(RuntimeError{
                VMError{RuntimeErrorCode::TypeError, "process port is not writable"}});
        }

#ifdef _WIN32
        std::size_t offset = 0;
        while (offset < size) {
            const auto chunk = static_cast<DWORD>((std::min)(size - offset, static_cast<std::size_t>(0x7fffffff)));
            DWORD written = 0;
            if (!::WriteFile(handle_, data + offset, chunk, &written, nullptr)) {
                return std::unexpected(RuntimeError{
                    VMError{RuntimeErrorCode::InternalError, "process port write failed"}});
            }
            offset += static_cast<std::size_t>(written);
        }
        return {};
#else
        std::size_t offset = 0;
        while (offset < size) {
            const ssize_t n = ::write(handle_, data + offset, size - offset);
            if (n > 0) {
                offset += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            return std::unexpected(RuntimeError{
                VMError{RuntimeErrorCode::InternalError, "process port write failed"}});
        }
        return {};
#endif
    }

    NativePipeHandle handle_{kInvalidPipeHandle};
    Direction direction_{Direction::Input};
    bool open_{false};
};

struct SpawnResult {
#ifdef _WIN32
    HANDLE process_handle{nullptr};
    DWORD pid{0};
#else
    pid_t pid{-1};
#endif
    NativePipeHandle stdin_parent{kInvalidPipeHandle};
    NativePipeHandle stdout_parent{kInvalidPipeHandle};
    NativePipeHandle stderr_parent{kInvalidPipeHandle};
};

#ifdef _WIN32
std::expected<void, RuntimeError> create_pipe_pair(
    ScopedPipeHandle& parent_end,
    ScopedPipeHandle& child_end,
    bool parent_reads,
    const char* who) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!::CreatePipe(&read_end, &write_end, &sa, 0)) {
        return prefixed_error(kSpawnFailedPrefix, std::string(who) + ": CreatePipe failed");
    }

    if (parent_reads) {
        parent_end.reset(read_end);
        child_end.reset(write_end);
    } else {
        parent_end.reset(write_end);
        child_end.reset(read_end);
    }

    if (!::SetHandleInformation(parent_end.get(), HANDLE_FLAG_INHERIT, 0)) {
        return prefixed_error(kSpawnFailedPrefix, std::string(who) + ": SetHandleInformation failed");
    }
    return {};
}

std::expected<ScopedPipeHandle, RuntimeError> duplicate_inheritable_handle(const HANDLE source, const char* who) {
    if (source == nullptr || source == INVALID_HANDLE_VALUE) {
        return prefixed_error(kSpawnFailedPrefix, std::string(who) + ": invalid standard handle");
    }
    HANDLE duplicated = nullptr;
    if (!::DuplicateHandle(
            ::GetCurrentProcess(),
            source,
            ::GetCurrentProcess(),
            &duplicated,
            0,
            TRUE,
            DUPLICATE_SAME_ACCESS)) {
        return prefixed_error(kSpawnFailedPrefix, std::string(who) + ": DuplicateHandle failed");
    }
    return ScopedPipeHandle(duplicated);
}

std::expected<ScopedPipeHandle, RuntimeError> open_null_handle(const DWORD access, const char* who) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    HANDLE handle = ::CreateFileW(
        L"NUL",
        access,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return prefixed_error(kSpawnFailedPrefix, std::string(who) + ": failed to open NUL");
    }
    return ScopedPipeHandle(handle);
}
#else
std::expected<void, RuntimeError> create_pipe_pair(
    ScopedPipeHandle& parent_end,
    ScopedPipeHandle& child_end,
    bool parent_reads,
    const char* who) {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        return prefixed_error(kSpawnFailedPrefix, std::string(who) + ": pipe() failed");
    }
    if (parent_reads) {
        parent_end.reset(fds[0]);
        child_end.reset(fds[1]);
    } else {
        parent_end.reset(fds[1]);
        child_end.reset(fds[0]);
    }
    return {};
}

std::expected<ScopedPipeHandle, RuntimeError> open_null_handle(const int flags, const char* who) {
    const int fd = ::open("/dev/null", flags);
    if (fd < 0) {
        return prefixed_error(kSpawnFailedPrefix, std::string(who) + ": failed to open /dev/null");
    }
    return ScopedPipeHandle(fd);
}
#endif

std::expected<SpawnResult, RuntimeError> spawn_child_process(
    const std::string& program,
    const std::vector<std::string>& args,
    const ProcessOptions& options) {
    SpawnResult result;
    ScopedPipeHandle stdin_parent;
    ScopedPipeHandle stdin_child;
    ScopedPipeHandle stdout_parent;
    ScopedPipeHandle stdout_child;
    ScopedPipeHandle stderr_parent;
    ScopedPipeHandle stderr_child;

#ifdef _WIN32
    ScopedPipeHandle child_stdin_handle;
    ScopedPipeHandle child_stdout_handle;
    ScopedPipeHandle child_stderr_handle;

    if (options.stdin_mode == StdinMode::Pipe || options.stdin_mode == StdinMode::Data) {
        auto pipe_res = create_pipe_pair(stdin_parent, stdin_child, false, "stdin");
        if (!pipe_res) return std::unexpected(pipe_res.error());
        child_stdin_handle = std::move(stdin_child);
    } else if (options.stdin_mode == StdinMode::Null) {
        auto nul = open_null_handle(GENERIC_READ, "stdin");
        if (!nul) return std::unexpected(nul.error());
        child_stdin_handle = std::move(*nul);
    } else {
        auto dup = duplicate_inheritable_handle(::GetStdHandle(STD_INPUT_HANDLE), "stdin");
        if (!dup) return std::unexpected(dup.error());
        child_stdin_handle = std::move(*dup);
    }

    if (options.stdout_mode == StreamMode::Pipe || options.stdout_mode == StreamMode::Capture) {
        auto pipe_res = create_pipe_pair(stdout_parent, stdout_child, true, "stdout");
        if (!pipe_res) return std::unexpected(pipe_res.error());
        child_stdout_handle = std::move(stdout_child);
    } else if (options.stdout_mode == StreamMode::Null) {
        auto nul = open_null_handle(GENERIC_WRITE, "stdout");
        if (!nul) return std::unexpected(nul.error());
        child_stdout_handle = std::move(*nul);
    } else {
        auto dup = duplicate_inheritable_handle(::GetStdHandle(STD_OUTPUT_HANDLE), "stdout");
        if (!dup) return std::unexpected(dup.error());
        child_stdout_handle = std::move(*dup);
    }

    if (options.stderr_mode == StreamMode::Merge) {
        auto dup = duplicate_inheritable_handle(child_stdout_handle.get(), "stderr");
        if (!dup) return std::unexpected(dup.error());
        child_stderr_handle = std::move(*dup);
    } else if (options.stderr_mode == StreamMode::Pipe || options.stderr_mode == StreamMode::Capture) {
        auto pipe_res = create_pipe_pair(stderr_parent, stderr_child, true, "stderr");
        if (!pipe_res) return std::unexpected(pipe_res.error());
        child_stderr_handle = std::move(stderr_child);
    } else if (options.stderr_mode == StreamMode::Null) {
        auto nul = open_null_handle(GENERIC_WRITE, "stderr");
        if (!nul) return std::unexpected(nul.error());
        child_stderr_handle = std::move(*nul);
    } else {
        auto dup = duplicate_inheritable_handle(::GetStdHandle(STD_ERROR_HANDLE), "stderr");
        if (!dup) return std::unexpected(dup.error());
        child_stderr_handle = std::move(*dup);
    }

    std::vector<wchar_t> env_block;
    LPVOID env_ptr = nullptr;
    if (options.use_environment) {
        auto block = build_environment_block(options);
        if (!block) return std::unexpected(block.error());
        env_block = std::move(*block);
        env_ptr = static_cast<void*>(env_block.data());
    }

    std::wstring cwd_wide;
    LPCWSTR cwd_ptr = nullptr;
    if (options.cwd.has_value()) {
        cwd_wide = utf8_to_utf16(*options.cwd);
        cwd_ptr = cwd_wide.c_str();
    }

    std::wstring cmdline = build_command_line(program, args);
    std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_stdin_handle.get();
    si.hStdOutput = child_stdout_handle.get();
    si.hStdError = child_stderr_handle.get();

    PROCESS_INFORMATION pi{};
    const BOOL created = ::CreateProcessW(
        nullptr,
        cmdline_buf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_UNICODE_ENVIRONMENT,
        env_ptr,
        cwd_ptr,
        &si,
        &pi);
    if (!created) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return prefixed_error(kNotFoundPrefix, win32_error_message(err));
        }
        return prefixed_error(kSpawnFailedPrefix, win32_error_message(err));
    }

    ::CloseHandle(pi.hThread);
    result.process_handle = pi.hProcess;
    result.pid = pi.dwProcessId;
    result.stdin_parent = stdin_parent.release();
    result.stdout_parent = stdout_parent.release();
    result.stderr_parent = stderr_parent.release();
    return result;
#else
    if (options.stdin_mode == StdinMode::Pipe || options.stdin_mode == StdinMode::Data) {
        auto pipe_res = create_pipe_pair(stdin_parent, stdin_child, false, "stdin");
        if (!pipe_res) return std::unexpected(pipe_res.error());
    }
    if (options.stdout_mode == StreamMode::Pipe || options.stdout_mode == StreamMode::Capture) {
        auto pipe_res = create_pipe_pair(stdout_parent, stdout_child, true, "stdout");
        if (!pipe_res) return std::unexpected(pipe_res.error());
    }
    if (options.stderr_mode == StreamMode::Pipe || options.stderr_mode == StreamMode::Capture) {
        auto pipe_res = create_pipe_pair(stderr_parent, stderr_child, true, "stderr");
        if (!pipe_res) return std::unexpected(pipe_res.error());
    }

    ScopedPipeHandle null_stdin;
    ScopedPipeHandle null_stdout;
    ScopedPipeHandle null_stderr;
    if (options.stdin_mode == StdinMode::Null) {
        auto nul = open_null_handle(O_RDONLY, "stdin");
        if (!nul) return std::unexpected(nul.error());
        null_stdin = std::move(*nul);
    }
    if (options.stdout_mode == StreamMode::Null) {
        auto nul = open_null_handle(O_WRONLY, "stdout");
        if (!nul) return std::unexpected(nul.error());
        null_stdout = std::move(*nul);
    }
    if (options.stderr_mode == StreamMode::Null) {
        auto nul = open_null_handle(O_WRONLY, "stderr");
        if (!nul) return std::unexpected(nul.error());
        null_stderr = std::move(*nul);
    }

    int err_pipe[2] = {-1, -1};
    if (::pipe(err_pipe) != 0) {
        return prefixed_error(kSpawnFailedPrefix, "failed to create spawn error pipe");
    }
    ScopedPipeHandle err_read(err_pipe[0]);
    ScopedPipeHandle err_write(err_pipe[1]);

    const int cloexec_flags = ::fcntl(err_write.get(), F_GETFD);
    if (cloexec_flags >= 0) {
        ::fcntl(err_write.get(), F_SETFD, cloexec_flags | FD_CLOEXEC);
    }

    const auto env_storage = build_posix_environment(options);
    const std::string env_path = lookup_env_path(env_storage);

    const pid_t pid = ::fork();
    if (pid < 0) {
        return prefixed_error(kSpawnFailedPrefix, "fork() failed");
    }

    if (pid == 0) {
        err_read.reset();

        auto child_fail = [&](int code) -> void {
            const int err = (code == 0) ? errno : code;
            (void)::write(err_write.get(), &err, sizeof(err));
            ::_exit(127);
        };

        if (options.cwd.has_value() && ::chdir(options.cwd->c_str()) != 0) {
            child_fail(errno);
        }

        if (stdin_parent.valid()) stdin_parent.reset();
        if (stdout_parent.valid()) stdout_parent.reset();
        if (stderr_parent.valid()) stderr_parent.reset();

        if (options.stdin_mode == StdinMode::Pipe || options.stdin_mode == StdinMode::Data) {
            if (::dup2(stdin_child.get(), STDIN_FILENO) < 0) child_fail(errno);
        } else if (options.stdin_mode == StdinMode::Null) {
            if (::dup2(null_stdin.get(), STDIN_FILENO) < 0) child_fail(errno);
        }

        if (options.stdout_mode == StreamMode::Pipe || options.stdout_mode == StreamMode::Capture) {
            if (::dup2(stdout_child.get(), STDOUT_FILENO) < 0) child_fail(errno);
        } else if (options.stdout_mode == StreamMode::Null) {
            if (::dup2(null_stdout.get(), STDOUT_FILENO) < 0) child_fail(errno);
        }

        if (options.stderr_mode == StreamMode::Merge) {
            if (::dup2(STDOUT_FILENO, STDERR_FILENO) < 0) child_fail(errno);
        } else if (options.stderr_mode == StreamMode::Pipe || options.stderr_mode == StreamMode::Capture) {
            if (::dup2(stderr_child.get(), STDERR_FILENO) < 0) child_fail(errno);
        } else if (options.stderr_mode == StreamMode::Null) {
            if (::dup2(null_stderr.get(), STDERR_FILENO) < 0) child_fail(errno);
        }

        stdin_child.reset();
        stdout_child.reset();
        stderr_child.reset();
        null_stdin.reset();
        null_stdout.reset();
        null_stderr.reset();

        std::vector<std::string> argv_storage;
        argv_storage.reserve(args.size() + 1);
        argv_storage.push_back(program);
        argv_storage.insert(argv_storage.end(), args.begin(), args.end());

        std::vector<char*> argv;
        argv.reserve(argv_storage.size() + 1);
        for (auto& arg : argv_storage) argv.push_back(arg.data());
        argv.push_back(nullptr);

        if (!options.use_environment) {
            ::execvp(program.c_str(), argv.data());
            child_fail(errno);
        }

        std::vector<char*> envp;
        envp.reserve(env_storage.size() + 1);
        for (const auto& item : env_storage) {
            envp.push_back(const_cast<char*>(item.c_str()));
        }
        envp.push_back(nullptr);

        if (program.find('/') != std::string::npos) {
            ::execve(program.c_str(), argv.data(), envp.data());
            child_fail(errno);
        }

        const std::string path_value = env_path.empty() ? "/usr/bin:/bin" : env_path;
        int last_errno = ENOENT;
        std::size_t start = 0;
        while (start <= path_value.size()) {
            const auto sep = path_value.find(':', start);
            const std::string dir = sep == std::string::npos
                ? path_value.substr(start)
                : path_value.substr(start, sep - start);
            std::string candidate = dir.empty() ? "." : dir;
            candidate.push_back('/');
            candidate += program;
            ::execve(candidate.c_str(), argv.data(), envp.data());
            if (errno != ENOENT && errno != ENOTDIR) {
                last_errno = errno;
                if (errno != EACCES) break;
            }
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
        child_fail(last_errno);
    }

    err_write.reset();
    stdin_child.reset();
    stdout_child.reset();
    stderr_child.reset();

    int child_errno = 0;
    ssize_t read_n = 0;
    while (true) {
        read_n = ::read(err_read.get(), &child_errno, sizeof(child_errno));
        if (read_n < 0 && errno == EINTR) continue;
        break;
    }
    err_read.reset();

    if (read_n > 0) {
        int status = 0;
        (void)::waitpid(pid, &status, 0);
        if (child_errno == ENOENT) {
            return prefixed_error(kNotFoundPrefix, std::strerror(child_errno));
        }
        return prefixed_error(kSpawnFailedPrefix, std::strerror(child_errno));
    }

    result.pid = pid;
    result.stdin_parent = stdin_parent.release();
    result.stdout_parent = stdout_parent.release();
    result.stderr_parent = stderr_parent.release();
    return result;
#endif
}

std::expected<types::ProcessHandleObject*, RuntimeError> require_process_handle_object(
    Heap& heap,
    LispVal value,
    const char* who) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
        return type_error(std::string(who) + ": expected process handle");
    }
    auto* obj = heap.try_get_as<ObjectKind::ProcessHandle, types::ProcessHandleObject>(ops::payload(value));
    if (!obj || !obj->handle) {
        return type_error(std::string(who) + ": expected process handle");
    }
    return obj;
}

int decode_exit_status(const int status) {
#ifdef _WIN32
    return status;
#else
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
}

bool poll_process_exit(const std::shared_ptr<types::ProcessHandle>& handle) {
#ifdef _WIN32
    if (!handle || handle->process_handle == nullptr || handle->process_handle == INVALID_HANDLE_VALUE) {
        handle->running.store(false, std::memory_order_release);
        return false;
    }
    DWORD code = STILL_ACTIVE;
    if (!::GetExitCodeProcess(handle->process_handle, &code)) {
        return handle->running.load(std::memory_order_acquire);
    }
    if (code == STILL_ACTIVE) return true;
    handle->running.store(false, std::memory_order_release);
    handle->waited.store(true, std::memory_order_release);
    handle->exit_code.store(static_cast<int>(code), std::memory_order_release);
    return false;
#else
    if (!handle || handle->pid <= 0) {
        handle->running.store(false, std::memory_order_release);
        return false;
    }
    int status = 0;
    const pid_t r = ::waitpid(handle->pid, &status, WNOHANG);
    if (r == 0) return true;
    if (r == handle->pid) {
        handle->running.store(false, std::memory_order_release);
        handle->waited.store(true, std::memory_order_release);
        handle->exit_code.store(decode_exit_status(status), std::memory_order_release);
        handle->pid = -1;
        return false;
    }
    if (r < 0 && errno == ECHILD) {
        handle->running.store(false, std::memory_order_release);
        handle->waited.store(true, std::memory_order_release);
        if (handle->exit_code.load(std::memory_order_acquire) < 0) {
            handle->exit_code.store(-1, std::memory_order_release);
        }
        handle->pid = -1;
        return false;
    }
    return handle->running.load(std::memory_order_acquire);
#endif
}

std::expected<std::optional<int>, RuntimeError> wait_for_process(
    const std::shared_ptr<types::ProcessHandle>& handle,
    const std::optional<std::int64_t> timeout_ms) {
    if (!handle) return prefixed_error(kWaitFailedPrefix, "invalid process handle");

    const int cached_exit = handle->exit_code.load(std::memory_order_acquire);
    if (handle->waited.load(std::memory_order_acquire) && cached_exit >= 0) {
        return cached_exit;
    }

#ifdef _WIN32
    if (handle->process_handle == nullptr || handle->process_handle == INVALID_HANDLE_VALUE) {
        return prefixed_error(kWaitFailedPrefix, "process handle is closed");
    }

    DWORD timeout = INFINITE;
    if (timeout_ms.has_value()) {
        timeout = static_cast<DWORD>((std::min)(*timeout_ms, static_cast<std::int64_t>(0x7fffffff)));
    }

    const DWORD wait_code = ::WaitForSingleObject(handle->process_handle, timeout);
    if (wait_code == WAIT_TIMEOUT) return std::optional<int>{};
    if (wait_code != WAIT_OBJECT_0) {
        return prefixed_error(kWaitFailedPrefix, win32_error_message(::GetLastError()));
    }

    DWORD code = STILL_ACTIVE;
    if (!::GetExitCodeProcess(handle->process_handle, &code)) {
        return prefixed_error(kWaitFailedPrefix, win32_error_message(::GetLastError()));
    }

    handle->running.store(false, std::memory_order_release);
    handle->waited.store(true, std::memory_order_release);
    handle->exit_code.store(static_cast<int>(code), std::memory_order_release);
    return static_cast<int>(code);
#else
    if (handle->pid <= 0) {
        if (handle->waited.load(std::memory_order_acquire)) {
            return std::optional<int>(handle->exit_code.load(std::memory_order_acquire));
        }
        return prefixed_error(kWaitFailedPrefix, "invalid process id");
    }

    auto do_wait = [&](const int flags) -> std::expected<std::optional<int>, RuntimeError> {
        int status = 0;
        while (true) {
            const pid_t r = ::waitpid(handle->pid, &status, flags);
            if (r == 0) return std::optional<int>{};
            if (r == handle->pid) {
                const int decoded = decode_exit_status(status);
                handle->running.store(false, std::memory_order_release);
                handle->waited.store(true, std::memory_order_release);
                handle->exit_code.store(decoded, std::memory_order_release);
                handle->pid = -1;
                return decoded;
            }
            if (r < 0 && errno == EINTR) continue;
            if (r < 0 && errno == ECHILD) {
                handle->running.store(false, std::memory_order_release);
                handle->waited.store(true, std::memory_order_release);
                handle->pid = -1;
                return handle->exit_code.load(std::memory_order_acquire);
            }
            return prefixed_error(kWaitFailedPrefix, std::strerror(errno));
        }
    };

    if (!timeout_ms.has_value()) {
        return do_wait(0);
    }

    const auto timeout = std::chrono::milliseconds(*timeout_ms);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        auto res = do_wait(WNOHANG);
        if (!res) return std::unexpected(res.error());
        if (res->has_value()) return *res;
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::optional<int>{};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
#endif
}

std::expected<bool, RuntimeError> signal_process(
    const std::shared_ptr<types::ProcessHandle>& handle,
    const bool hard_signal) {
    if (!handle) return prefixed_error(kWaitFailedPrefix, "invalid process handle");

#ifdef _WIN32
    if (handle->process_handle == nullptr || handle->process_handle == INVALID_HANDLE_VALUE) return false;
    if (!::TerminateProcess(handle->process_handle, hard_signal ? 1u : 0u)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_ACCESS_DENIED) return false;
        return prefixed_error(kWaitFailedPrefix, win32_error_message(err));
    }
    handle->running.store(false, std::memory_order_release);
    return true;
#else
    if (handle->pid <= 0) return false;
    const int sig = hard_signal ? SIGKILL : SIGTERM;
    if (::kill(handle->pid, sig) != 0) {
        if (errno == ESRCH) {
            handle->running.store(false, std::memory_order_release);
            return false;
        }
        return prefixed_error(kWaitFailedPrefix, std::strerror(errno));
    }
    return true;
#endif
}

std::expected<LispVal, RuntimeError> bool_to_lisp(const bool value) {
    return value ? True : False;
}

std::expected<LispVal, RuntimeError> make_exit_code_value(Heap& heap, int code) {
    return make_fixnum(heap, static_cast<std::int64_t>(code));
}

std::expected<void, RuntimeError> validate_run_options(const ProcessOptions& options) {
    if (options.stdin_mode == StdinMode::Pipe) {
        return type_error("%process-run: stdin must be string, bytevector, 'inherit, or 'null");
    }
    if (options.stdout_mode == StreamMode::Pipe) {
        return type_error("%process-run: stdout must be 'capture, 'inherit, or 'null");
    }
    if (options.stderr_mode == StreamMode::Pipe) {
        return type_error("%process-run: stderr must be 'capture, 'inherit, 'null, or 'merge");
    }
    return {};
}

std::string decode_utf8_lossy(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return {};

    const std::string input(
        reinterpret_cast<const char*>(bytes.data()),
        reinterpret_cast<const char*>(bytes.data()) + bytes.size());

    std::string output;
    output.reserve(input.size());

    std::size_t pos = 0;
    while (pos < input.size()) {
        const std::size_t start = pos;
        auto decoded = utf8::decode(input, pos);
        if (decoded.has_value()) {
            output += utf8::encode(*decoded);
        } else {
            output += "\xEF\xBF\xBD";
            pos = start + 1;
        }
    }

    return output;
}

std::expected<void, RuntimeError> read_pipe_to_buffer(
    NativePipeHandle handle,
    std::vector<std::uint8_t>& out,
    const char* stream_name) {
    ScopedPipeHandle scoped(handle);
    if (!scoped.valid()) return {};

    std::array<std::uint8_t, 4096> buffer{};

#ifdef _WIN32
    while (true) {
        DWORD bytes_read = 0;
        const BOOL ok = ::ReadFile(
            scoped.get(),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes_read,
            nullptr);
        if (ok) {
            if (bytes_read == 0) break;
            out.insert(out.end(), buffer.data(), buffer.data() + bytes_read);
            continue;
        }

        const DWORD err = ::GetLastError();
        if (err == ERROR_BROKEN_PIPE) break;
        return prefixed_error(
            kWaitFailedPrefix,
            std::string("failed to read ") + stream_name + ": " + win32_error_message(err));
    }
#else
    while (true) {
        const ssize_t n = ::read(scoped.get(), buffer.data(), buffer.size());
        if (n > 0) {
            out.insert(out.end(), buffer.data(), buffer.data() + static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        return prefixed_error(
            kWaitFailedPrefix,
            std::string("failed to read ") + stream_name + ": " + std::strerror(errno));
    }
#endif

    return {};
}

std::expected<void, RuntimeError> write_pipe_from_buffer(
    NativePipeHandle handle,
    std::span<const std::uint8_t> data) {
    ScopedPipeHandle scoped(handle);
    if (!scoped.valid()) return {};

    std::size_t offset = 0;

#ifdef _WIN32
    while (offset < data.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(
            data.size() - offset,
            static_cast<std::size_t>(0x7fffffff)));
        DWORD written = 0;
        if (!::WriteFile(scoped.get(), data.data() + offset, chunk, &written, nullptr)) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) return {};
            return prefixed_error(
                kWaitFailedPrefix,
                "failed to write process stdin: " + win32_error_message(err));
        }
        if (written == 0) {
            return prefixed_error(kWaitFailedPrefix, "failed to write process stdin: wrote 0 bytes");
        }
        offset += written;
    }
#else
    while (offset < data.size()) {
        const ssize_t n = ::write(scoped.get(), data.data() + offset, data.size() - offset);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EPIPE) return {};
        return prefixed_error(
            kWaitFailedPrefix,
            "failed to write process stdin: " + std::string(std::strerror(errno)));
    }
#endif

    return {};
}

std::expected<LispVal, RuntimeError> make_captured_stream_value(
    Heap& heap,
    InternTable& intern_table,
    StreamMode mode,
    bool binary,
    std::span<const std::uint8_t> data) {
    if (mode != StreamMode::Capture) return False;
    if (binary) {
        return make_bytevector(heap, std::vector<std::uint8_t>(data.begin(), data.end()));
    }
    return make_string(heap, intern_table, decode_utf8_lossy(data));
}

std::expected<LispVal, RuntimeError> make_three_list(
    Heap& heap,
    LispVal first,
    LispVal second,
    LispVal third) {
    auto tail = make_cons(heap, third, Nil);
    if (!tail) return std::unexpected(tail.error());
    auto middle = make_cons(heap, second, *tail);
    if (!middle) return std::unexpected(middle.error());
    return make_cons(heap, first, *middle);
}

} // namespace

void register_process_primitives(BuiltinEnvironment& env,
                                 Heap& heap,
                                 InternTable& intern_table,
                                 vm::VM& /*vm*/) {
    env.register_builtin("%process-run", 2, true, [&heap, &intern_table](Args args)
        -> std::expected<LispVal, RuntimeError> {
        if (args.size() < 2 || args.size() > 3) {
            return invalid_arity("%process-run: expected program args [opts]");
        }

        auto program = require_string_arg(intern_table, args[0], "%process-run", "program");
        if (!program) return std::unexpected(program.error());
        auto argv = require_string_list(heap, intern_table, args[1], "%process-run", "args");
        if (!argv) return std::unexpected(argv.error());

        ParseDefaults defaults{};
        defaults.stdin_mode = StdinMode::Null;
        defaults.stdout_mode = StreamMode::Capture;
        defaults.stderr_mode = StreamMode::Capture;

        auto options = parse_process_options(
            heap,
            intern_table,
            args.size() == 3 ? args[2] : Nil,
            "%process-run",
            defaults);
        if (!options) return std::unexpected(options.error());

        auto options_check = validate_run_options(*options);
        if (!options_check) return std::unexpected(options_check.error());

        auto spawned = spawn_child_process(*program, *argv, *options);
        if (!spawned) return std::unexpected(spawned.error());

        auto state = std::make_shared<types::ProcessHandle>();
#ifdef _WIN32
        state->process_handle = spawned->process_handle;
        state->pid = spawned->pid;
#else
        state->pid = spawned->pid;
#endif

        ScopedPipeHandle stdin_parent(spawned->stdin_parent);
        ScopedPipeHandle stdout_parent(spawned->stdout_parent);
        ScopedPipeHandle stderr_parent(spawned->stderr_parent);

        std::vector<std::uint8_t> stdout_capture;
        std::vector<std::uint8_t> stderr_capture;
        std::optional<RuntimeError> io_error;
        std::mutex io_error_mutex;
        auto save_io_error = [&io_error, &io_error_mutex](RuntimeError err) {
            std::scoped_lock lock(io_error_mutex);
            if (!io_error.has_value()) io_error = std::move(err);
        };

        std::thread stdin_writer;
        if (options->stdin_mode == StdinMode::Data && stdin_parent.valid()) {
            const auto stdin_handle = stdin_parent.release();
            auto input_data = options->stdin_data;
            stdin_writer = std::thread([stdin_handle, input_data = std::move(input_data), &save_io_error]() mutable {
                auto write_result = write_pipe_from_buffer(stdin_handle, input_data);
                if (!write_result) save_io_error(write_result.error());
            });
        } else if (stdin_parent.valid()) {
            stdin_parent.reset();
        }

        std::thread stdout_reader;
        if (options->stdout_mode == StreamMode::Capture && stdout_parent.valid()) {
            const auto stdout_handle = stdout_parent.release();
            stdout_reader = std::thread([stdout_handle, &stdout_capture, &save_io_error]() {
                auto read_result = read_pipe_to_buffer(stdout_handle, stdout_capture, "stdout");
                if (!read_result) save_io_error(read_result.error());
            });
        } else if (stdout_parent.valid()) {
            stdout_parent.reset();
        }

        std::thread stderr_reader;
        if (options->stderr_mode == StreamMode::Capture && stderr_parent.valid()) {
            const auto stderr_handle = stderr_parent.release();
            stderr_reader = std::thread([stderr_handle, &stderr_capture, &save_io_error]() {
                auto read_result = read_pipe_to_buffer(stderr_handle, stderr_capture, "stderr");
                if (!read_result) save_io_error(read_result.error());
            });
        } else if (stderr_parent.valid()) {
            stderr_parent.reset();
        }

        std::optional<int> exit_code;
        std::optional<RuntimeError> run_error;

        auto wait_result = wait_for_process(state, options->timeout_ms);
        if (!wait_result) {
            run_error = wait_result.error();
        } else if (!wait_result->has_value()) {
            (void)signal_process(state, true);
            (void)wait_for_process(state, std::nullopt);

            std::string timeout_detail = "timed out";
            if (options->timeout_ms.has_value()) {
                timeout_detail += " after " + std::to_string(*options->timeout_ms) + " ms";
            }
            run_error = RuntimeError{
                VMError{RuntimeErrorCode::InternalError,
                        std::string(kTimeoutPrefix) + ": " + timeout_detail}
            };
        } else {
            exit_code = **wait_result;
        }

        if (stdin_writer.joinable()) stdin_writer.join();
        if (stdout_reader.joinable()) stdout_reader.join();
        if (stderr_reader.joinable()) stderr_reader.join();

        if (run_error.has_value()) return std::unexpected(*run_error);
        if (io_error.has_value()) return std::unexpected(*io_error);
        if (!exit_code.has_value()) {
            return prefixed_error(kWaitFailedPrefix, "process exited without status");
        }

        auto exit_code_value = make_exit_code_value(heap, *exit_code);
        if (!exit_code_value) return std::unexpected(exit_code_value.error());

        auto stdout_value = make_captured_stream_value(
            heap,
            intern_table,
            options->stdout_mode,
            options->binary,
            stdout_capture);
        if (!stdout_value) return std::unexpected(stdout_value.error());

        auto stderr_value = make_captured_stream_value(
            heap,
            intern_table,
            options->stderr_mode,
            options->binary,
            stderr_capture);
        if (!stderr_value) return std::unexpected(stderr_value.error());

        return make_three_list(heap, *exit_code_value, *stdout_value, *stderr_value);
    });

    env.register_builtin("%process-spawn", 2, true, [&heap, &intern_table](Args args)
        -> std::expected<LispVal, RuntimeError> {
        if (args.size() < 2 || args.size() > 3) {
            return invalid_arity("%process-spawn: expected program args [opts]");
        }

        auto program = require_string_arg(intern_table, args[0], "%process-spawn", "program");
        if (!program) return std::unexpected(program.error());
        auto argv = require_string_list(heap, intern_table, args[1], "%process-spawn", "args");
        if (!argv) return std::unexpected(argv.error());

        ParseDefaults defaults{};
        defaults.stdin_mode = StdinMode::Pipe;
        defaults.stdout_mode = StreamMode::Pipe;
        defaults.stderr_mode = StreamMode::Pipe;

        auto options = parse_process_options(
            heap,
            intern_table,
            args.size() == 3 ? args[2] : Nil,
            "%process-spawn",
            defaults);
        if (!options) return std::unexpected(options.error());

        auto spawned = spawn_child_process(*program, *argv, *options);
        if (!spawned) return std::unexpected(spawned.error());

        auto state = std::make_shared<types::ProcessHandle>();
#ifdef _WIN32
        state->process_handle = spawned->process_handle;
        state->pid = spawned->pid;
#else
        state->pid = spawned->pid;
#endif

        ScopedPipeHandle stdin_parent(spawned->stdin_parent);
        ScopedPipeHandle stdout_parent(spawned->stdout_parent);
        ScopedPipeHandle stderr_parent(spawned->stderr_parent);

        if ((options->stdin_mode == StdinMode::Pipe || options->stdin_mode == StdinMode::Data)
            && stdin_parent.valid()) {
            if (options->stdin_mode == StdinMode::Data) {
                NativePipePort writer(stdin_parent.get(), NativePipePort::Direction::Output);
                auto write_res = writer.write_bytes(options->stdin_data);
                if (!write_res) return std::unexpected(write_res.error());
                (void)writer.close();
            } else {
                auto stdin_port = make_port(heap, std::make_shared<NativePipePort>(
                    stdin_parent.release(), NativePipePort::Direction::Output));
                if (!stdin_port) return std::unexpected(stdin_port.error());
                state->stdin_port = *stdin_port;
            }
        }

        if ((options->stdout_mode == StreamMode::Pipe || options->stdout_mode == StreamMode::Capture)
            && stdout_parent.valid()) {
            auto stdout_port = make_port(heap, std::make_shared<NativePipePort>(
                stdout_parent.release(), NativePipePort::Direction::Input));
            if (!stdout_port) return std::unexpected(stdout_port.error());
            state->stdout_port = *stdout_port;
        }

        if ((options->stderr_mode == StreamMode::Pipe || options->stderr_mode == StreamMode::Capture)
            && stderr_parent.valid()) {
            auto stderr_port = make_port(heap, std::make_shared<NativePipePort>(
                stderr_parent.release(), NativePipePort::Direction::Input));
            if (!stderr_port) return std::unexpected(stderr_port.error());
            state->stderr_port = *stderr_port;
        }

        return make_process_handle(heap, std::move(state));
    });

    env.register_builtin("%process-wait", 1, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.size() > 2) {
            return invalid_arity("%process-wait: expected handle [timeout-ms]");
        }
        auto process_obj = require_process_handle_object(heap, args[0], "%process-wait");
        if (!process_obj) return std::unexpected(process_obj.error());

        std::optional<std::int64_t> timeout_ms;
        if (args.size() == 2) {
            auto parsed_timeout = require_non_negative_int(heap, args[1], "%process-wait", "timeout-ms");
            if (!parsed_timeout) return std::unexpected(parsed_timeout.error());
            timeout_ms = *parsed_timeout;
        }

        auto wait_result = wait_for_process((*process_obj)->handle, timeout_ms);
        if (!wait_result) return std::unexpected(wait_result.error());
        if (!wait_result->has_value()) return False;
        return make_exit_code_value(heap, **wait_result);
    });

    env.register_builtin("%process-kill", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto process_obj = require_process_handle_object(heap, args[0], "%process-kill");
        if (!process_obj) return std::unexpected(process_obj.error());
        auto killed = signal_process((*process_obj)->handle, true);
        if (!killed) return std::unexpected(killed.error());
        return bool_to_lisp(*killed);
    });

    env.register_builtin("%process-terminate", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto process_obj = require_process_handle_object(heap, args[0], "%process-terminate");
        if (!process_obj) return std::unexpected(process_obj.error());
        auto terminated = signal_process((*process_obj)->handle, false);
        if (!terminated) return std::unexpected(terminated.error());
        return bool_to_lisp(*terminated);
    });

    env.register_builtin("%process-pid", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto process_obj = require_process_handle_object(heap, args[0], "%process-pid");
        if (!process_obj) return std::unexpected(process_obj.error());
#ifdef _WIN32
        return make_fixnum(heap, static_cast<std::int64_t>((*process_obj)->handle->pid));
#else
        return make_fixnum(heap, static_cast<std::int64_t>((*process_obj)->handle->pid));
#endif
    });

    env.register_builtin("%process-alive?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto process_obj = require_process_handle_object(heap, args[0], "%process-alive?");
        if (!process_obj) return std::unexpected(process_obj.error());
        const bool alive = poll_process_exit((*process_obj)->handle);
        return bool_to_lisp(alive);
    });

    env.register_builtin("%process-exit-code", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto process_obj = require_process_handle_object(heap, args[0], "%process-exit-code");
        if (!process_obj) return std::unexpected(process_obj.error());

        if (poll_process_exit((*process_obj)->handle)) return False;
        const int code = (*process_obj)->handle->exit_code.load(std::memory_order_acquire);
        if (code < 0) return False;
        return make_exit_code_value(heap, code);
    });

    env.register_builtin("%process-handle?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) return False;
        auto* obj = heap.try_get_as<ObjectKind::ProcessHandle, types::ProcessHandleObject>(ops::payload(args[0]));
        return (obj && obj->handle) ? True : False;
    });

    env.register_builtin("%process-stdin-port", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto process_obj = require_process_handle_object(heap, args[0], "%process-stdin-port");
        if (!process_obj) return std::unexpected(process_obj.error());
        return ((*process_obj)->handle->stdin_port == Nil) ? False : (*process_obj)->handle->stdin_port;
    });

    env.register_builtin("%process-stdout-port", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto process_obj = require_process_handle_object(heap, args[0], "%process-stdout-port");
        if (!process_obj) return std::unexpected(process_obj.error());
        return ((*process_obj)->handle->stdout_port == Nil) ? False : (*process_obj)->handle->stdout_port;
    });

    env.register_builtin("%process-stderr-port", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto process_obj = require_process_handle_object(heap, args[0], "%process-stderr-port");
        if (!process_obj) return std::unexpected(process_obj.error());
        return ((*process_obj)->handle->stderr_port == Nil) ? False : (*process_obj)->handle->stderr_port;
    });
}

} // namespace eta::runtime
