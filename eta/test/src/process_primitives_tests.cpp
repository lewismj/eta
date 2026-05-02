#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <eta/runtime/builtin_env.h>
#include <eta/runtime/error.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/numeric_value.h>
#include <eta/runtime/port_primitives.h>
#include <eta/runtime/process_primitives.h>
#include <eta/runtime/string_view.h>
#include <eta/runtime/types/types.h>
#include <eta/runtime/vm/vm.h>

using namespace eta::runtime;
using namespace eta::runtime::error;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::nanbox;

namespace {
namespace fs = std::filesystem;

struct ShellCommand {
    std::string program;
    std::vector<std::string> args;
};

struct RunTuple {
    std::int64_t exit_code{0};
    LispVal stdout_value{Nil};
    LispVal stderr_value{Nil};
};

std::expected<LispVal, RuntimeError> call_builtin(
    BuiltinEnvironment& env,
    const std::string& name,
    const std::vector<LispVal>& args)
{
    auto idx = env.lookup(name);
    if (!idx) {
        return std::unexpected(RuntimeError{
            VMError{RuntimeErrorCode::InternalError, "missing builtin: " + name}});
    }
    return env.specs()[*idx].func(args);
}

LispVal make_int(std::int64_t value) {
    auto encoded = ops::encode<std::int64_t>(value);
    BOOST_REQUIRE(encoded.has_value());
    return *encoded;
}

LispVal make_sym(InternTable& intern_table, const std::string& text) {
    auto sym = make_symbol(intern_table, text);
    BOOST_REQUIRE(sym.has_value());
    return *sym;
}

LispVal make_str(Heap& heap, InternTable& intern_table, const std::string& text) {
    auto str = make_string(heap, intern_table, text);
    BOOST_REQUIRE(str.has_value());
    return *str;
}

LispVal make_list(Heap& heap, const std::vector<LispVal>& elements) {
    LispVal out = Nil;
    for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
        auto cell = make_cons(heap, *it, out);
        BOOST_REQUIRE(cell.has_value());
        out = *cell;
    }
    return out;
}

LispVal make_pair(Heap& heap, LispVal car, LispVal cdr) {
    auto pair = make_cons(heap, car, cdr);
    BOOST_REQUIRE(pair.has_value());
    return *pair;
}

LispVal make_string_list(
    Heap& heap,
    InternTable& intern_table,
    const std::vector<std::string>& values) {
    std::vector<LispVal> out;
    out.reserve(values.size());
    for (const auto& value : values) {
        out.push_back(make_str(heap, intern_table, value));
    }
    return make_list(heap, out);
}

LispVal make_alist(
    Heap& heap,
    InternTable& intern_table,
    const std::vector<std::pair<std::string, LispVal>>& pairs) {
    LispVal out = Nil;
    for (auto it = pairs.rbegin(); it != pairs.rend(); ++it) {
        LispVal key = make_sym(intern_table, it->first);
        LispVal pair = make_pair(heap, key, it->second);
        auto cell = make_cons(heap, pair, out);
        BOOST_REQUIRE(cell.has_value());
        out = *cell;
    }
    return out;
}

std::int64_t decode_int(Heap& heap, LispVal value) {
    auto n = classify_numeric(value, heap);
    BOOST_REQUIRE(n.is_valid());
    BOOST_REQUIRE(n.is_fixnum());
    return n.int_val;
}

std::string decode_string(InternTable& intern_table, LispVal value) {
    auto sv = StringView::try_from(value, intern_table);
    BOOST_REQUIRE(sv.has_value());
    return std::string(sv->view());
}

std::vector<std::uint8_t> decode_bytevector(Heap& heap, LispVal value) {
    BOOST_REQUIRE(ops::is_boxed(value));
    BOOST_REQUIRE(ops::tag(value) == Tag::HeapObject);
    auto* bv = heap.try_get_as<ObjectKind::ByteVector, types::ByteVector>(ops::payload(value));
    BOOST_REQUIRE(bv != nullptr);
    return bv->data;
}

RunTuple decode_run_tuple(Heap& heap, LispVal value) {
    BOOST_REQUIRE(ops::is_boxed(value));
    BOOST_REQUIRE(ops::tag(value) == Tag::HeapObject);
    auto* first = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(value));
    BOOST_REQUIRE(first != nullptr);

    BOOST_REQUIRE(ops::is_boxed(first->cdr));
    BOOST_REQUIRE(ops::tag(first->cdr) == Tag::HeapObject);
    auto* second = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(first->cdr));
    BOOST_REQUIRE(second != nullptr);

    BOOST_REQUIRE(ops::is_boxed(second->cdr));
    BOOST_REQUIRE(ops::tag(second->cdr) == Tag::HeapObject);
    auto* third = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(second->cdr));
    BOOST_REQUIRE(third != nullptr);
    BOOST_TEST(third->cdr == Nil);

    return RunTuple{
        .exit_code = decode_int(heap, first->car),
        .stdout_value = second->car,
        .stderr_value = third->car,
    };
}

void expect_internal_error_prefix(
    const std::expected<LispVal, RuntimeError>& result,
    std::string_view prefix) {
    BOOST_REQUIRE(!result.has_value());
    auto* vm_error = std::get_if<VMError>(&result.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_TEST(static_cast<int>(vm_error->code) ==
               static_cast<int>(RuntimeErrorCode::InternalError));
    const std::string expected_prefix = std::string(prefix) + ":";
    BOOST_TEST(vm_error->message.rfind(expected_prefix, 0) == 0);
}

ShellCommand shell_script(std::string script) {
#ifdef _WIN32
    return ShellCommand{"cmd", {"/c", std::move(script)}};
#else
    return ShellCommand{"/bin/sh", {"-c", std::move(script)}};
#endif
}

ShellCommand run_output_command() {
#ifdef _WIN32
    return shell_script("echo out& echo err 1>&2& exit /b 3");
#else
    return shell_script("printf 'out\\n'; printf 'err\\n' 1>&2; exit 3");
#endif
}

ShellCommand run_stdin_echo_command() {
#ifdef _WIN32
    return ShellCommand{"cmd", {"/v:on", "/c", "set /p LINE=& echo !LINE!"}};
#else
    return shell_script("IFS= read LINE; printf '%s\\n' \"$LINE\"");
#endif
}

ShellCommand run_sleep_command() {
#ifdef _WIN32
    return shell_script("ping -n 3 127.0.0.1 >nul");
#else
    return shell_script("sleep 1");
#endif
}

ShellCommand run_cat_command() {
#ifdef _WIN32
    return ShellCommand{"cmd", {"/c", "more"}};
#else
    return ShellCommand{"/bin/cat", {}};
#endif
}

ShellCommand run_pwd_command() {
#ifdef _WIN32
    return shell_script("cd");
#else
    return ShellCommand{"/bin/pwd", {}};
#endif
}

ShellCommand run_env_echo_command() {
#ifdef _WIN32
    return shell_script("echo %ETA_PROCESS_TEST_ENV%");
#else
    return shell_script("printf '%s\\n' \"$ETA_PROCESS_TEST_ENV\"");
#endif
}

std::string trim_line_endings(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

std::string normalize_path_text(std::string text) {
    text = trim_line_endings(std::move(text));
    if (text.empty()) return text;
    auto normalized = fs::path(text).lexically_normal().generic_string();
#ifdef _WIN32
    for (char& ch : normalized) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
#endif
    return normalized;
}

std::string bytes_to_string(const std::vector<std::uint8_t>& bytes) {
    return std::string(
        reinterpret_cast<const char*>(bytes.data()),
        reinterpret_cast<const char*>(bytes.data()) + bytes.size());
}

struct ProcessFixture {
    Heap heap;
    InternTable intern_table;
    vm::VM vm;
    BuiltinEnvironment env;

    ProcessFixture()
        : heap(1ull << 22),
          intern_table(),
          vm(heap, intern_table) {
        register_port_primitives(env, heap, intern_table, vm);
        register_process_primitives(env, heap, intern_table, vm);
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(process_primitives_tests, ProcessFixture)

BOOST_AUTO_TEST_CASE(registers_expected_builtins) {
    for (const char* name : {
             "%process-run",
             "%process-spawn",
             "%process-wait",
             "%process-kill",
             "%process-terminate",
             "%process-pid",
             "%process-alive?",
             "%process-exit-code",
             "%process-handle?",
             "%process-stdin-port",
             "%process-stdout-port",
             "%process-stderr-port",
         }) {
        BOOST_TEST(env.lookup(name).has_value());
    }
}

BOOST_AUTO_TEST_CASE(process_run_captures_stdout_stderr_and_exit_code) {
    const auto cmd = run_output_command();
    const auto program = make_str(heap, intern_table, cmd.program);
    const LispVal args = make_string_list(heap, intern_table, cmd.args);

    auto run_res = call_builtin(env, "%process-run", {program, args});
    BOOST_REQUIRE(run_res.has_value());

    const auto tuple = decode_run_tuple(heap, *run_res);
    BOOST_TEST(tuple.exit_code == 3);
    BOOST_TEST(decode_string(intern_table, tuple.stdout_value).find("out") != std::string::npos);
    BOOST_TEST(decode_string(intern_table, tuple.stderr_value).find("err") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(process_run_binary_capture_supports_stdin_data) {
    const auto cmd = run_stdin_echo_command();
    const auto program = make_str(heap, intern_table, cmd.program);
    const LispVal args = make_string_list(heap, intern_table, cmd.args);

    const LispVal options = make_alist(heap, intern_table, {
        {"stdin", make_str(heap, intern_table, "hello-process")},
        {"binary?", True},
    });

    auto run_res = call_builtin(env, "%process-run", {program, args, options});
    BOOST_REQUIRE(run_res.has_value());

    const auto tuple = decode_run_tuple(heap, *run_res);
    BOOST_TEST(tuple.exit_code == 0);
    auto stdout_bytes = decode_bytevector(heap, tuple.stdout_value);
    BOOST_TEST(bytes_to_string(stdout_bytes).find("hello-process") != std::string::npos);
    (void)decode_bytevector(heap, tuple.stderr_value);
}

BOOST_AUTO_TEST_CASE(process_run_timeout_has_stable_prefix) {
    const auto cmd = run_sleep_command();
    const auto program = make_str(heap, intern_table, cmd.program);
    const LispVal args = make_string_list(heap, intern_table, cmd.args);
    const LispVal options = make_alist(heap, intern_table, {
        {"timeout-ms", make_int(10)},
    });

    auto run_res = call_builtin(env, "%process-run", {program, args, options});
    expect_internal_error_prefix(run_res, "process-timeout");
}

BOOST_AUTO_TEST_CASE(process_run_missing_program_has_not_found_prefix) {
    const auto program = make_str(heap, intern_table, "eta-no-such-program-2718");
    const LispVal args = make_list(heap, {});

    auto run_res = call_builtin(env, "%process-run", {program, args});
    expect_internal_error_prefix(run_res, "process-not-found");
}

BOOST_AUTO_TEST_CASE(process_spawn_wait_and_lifecycle_state_work) {
    const auto cmd = run_sleep_command();
    const auto program = make_str(heap, intern_table, cmd.program);
    const LispVal args = make_string_list(heap, intern_table, cmd.args);

    auto spawn_res = call_builtin(env, "%process-spawn", {program, args});
    BOOST_REQUIRE(spawn_res.has_value());
    const LispVal handle = *spawn_res;

    auto pred = call_builtin(env, "%process-handle?", {handle});
    BOOST_REQUIRE(pred.has_value());
    BOOST_TEST(*pred == True);

    auto pid = call_builtin(env, "%process-pid", {handle});
    BOOST_REQUIRE(pid.has_value());
    BOOST_TEST(decode_int(heap, *pid) > 0);

    auto terminate_res = call_builtin(env, "%process-terminate", {handle});
    BOOST_REQUIRE(terminate_res.has_value());
    const bool terminate_is_bool = (*terminate_res == True) || (*terminate_res == False);
    BOOST_TEST(terminate_is_bool);

    auto waited = call_builtin(env, "%process-wait", {handle, make_int(3000)});
    BOOST_REQUIRE(waited.has_value());
    BOOST_TEST(*waited != False);
    (void)decode_int(heap, *waited);

    auto exit_code = call_builtin(env, "%process-exit-code", {handle});
    BOOST_REQUIRE(exit_code.has_value());
    BOOST_TEST(*exit_code != False);

    auto alive = call_builtin(env, "%process-alive?", {handle});
    BOOST_REQUIRE(alive.has_value());
    BOOST_TEST(*alive == False);
}

BOOST_AUTO_TEST_CASE(process_spawn_pipe_ports_round_trip_data) {
    const auto cmd = run_cat_command();
    const auto program = make_str(heap, intern_table, cmd.program);
    const LispVal args = make_string_list(heap, intern_table, cmd.args);

    auto spawn_res = call_builtin(env, "%process-spawn", {program, args});
    BOOST_REQUIRE(spawn_res.has_value());
    const LispVal handle = *spawn_res;

    auto stdin_port_res = call_builtin(env, "%process-stdin-port", {handle});
    auto stdout_port_res = call_builtin(env, "%process-stdout-port", {handle});
    BOOST_REQUIRE(stdin_port_res.has_value());
    BOOST_REQUIRE(stdout_port_res.has_value());
    BOOST_TEST(*stdin_port_res != False);
    BOOST_TEST(*stdout_port_res != False);

    const std::string payload = "eta-process-pipe\n";
    for (unsigned char ch : payload) {
        auto write_res = call_builtin(env, "write-u8", {make_int(ch), *stdin_port_res});
        BOOST_REQUIRE(write_res.has_value());
    }
    BOOST_REQUIRE(call_builtin(env, "close-port", {*stdin_port_res}).has_value());

    std::vector<std::uint8_t> captured;
    for (int i = 0; i < 4096; ++i) {
        auto byte_res = call_builtin(env, "read-u8", {*stdout_port_res});
        BOOST_REQUIRE(byte_res.has_value());
        if (*byte_res == False) break;
        captured.push_back(static_cast<std::uint8_t>(decode_int(heap, *byte_res)));
    }

    BOOST_REQUIRE(call_builtin(env, "%process-kill", {handle}).has_value());
    auto waited = call_builtin(env, "%process-wait", {handle, make_int(3000)});
    BOOST_REQUIRE(waited.has_value());
    BOOST_TEST(*waited != False);

    BOOST_TEST(bytes_to_string(captured).find("eta-process-pipe") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(process_run_respects_cwd_and_env_options) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path temp_dir = fs::temp_directory_path() / ("eta-process-test-" + std::to_string(stamp));
    fs::create_directories(temp_dir);

    const auto cwd_cmd = run_pwd_command();
    const auto cwd_program = make_str(heap, intern_table, cwd_cmd.program);
    const LispVal cwd_args = make_string_list(heap, intern_table, cwd_cmd.args);
    const LispVal cwd_opts = make_alist(heap, intern_table, {
        {"cwd", make_str(heap, intern_table, temp_dir.string())},
    });

    auto cwd_res = call_builtin(env, "%process-run", {cwd_program, cwd_args, cwd_opts});
    BOOST_REQUIRE(cwd_res.has_value());
    const auto cwd_tuple = decode_run_tuple(heap, *cwd_res);
    BOOST_TEST(cwd_tuple.exit_code == 0);

    const std::string expected_cwd = normalize_path_text(temp_dir.generic_string());
    const std::string actual_cwd = normalize_path_text(decode_string(intern_table, cwd_tuple.stdout_value));
    BOOST_TEST(actual_cwd == expected_cwd);

    const auto env_cmd = run_env_echo_command();
    const auto env_program = make_str(heap, intern_table, env_cmd.program);
    const LispVal env_args = make_string_list(heap, intern_table, env_cmd.args);
    const LispVal env_pairs = make_alist(heap, intern_table, {
        {"ETA_PROCESS_TEST_ENV", make_str(heap, intern_table, "eta-process-env")},
    });
    const LispVal env_opts = make_alist(heap, intern_table, {
        {"env", env_pairs},
    });

    auto env_res = call_builtin(env, "%process-run", {env_program, env_args, env_opts});
    BOOST_REQUIRE(env_res.has_value());
    const auto env_tuple = decode_run_tuple(heap, *env_res);
    BOOST_TEST(env_tuple.exit_code == 0);
    BOOST_TEST(decode_string(intern_table, env_tuple.stdout_value).find("eta-process-env") != std::string::npos);

    std::error_code ec;
    fs::remove_all(temp_dir, ec);
}

BOOST_AUTO_TEST_SUITE_END()
