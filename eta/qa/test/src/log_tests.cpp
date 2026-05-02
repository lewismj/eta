#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include <eta/log/log_primitives.h>
#include <eta/log/log_state.h>
#include <eta/runtime/builtin_env.h>
#include <eta/runtime/error.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/port.h>
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

std::string decode_symbol(InternTable& intern_table, LispVal value) {
    BOOST_REQUIRE(ops::is_boxed(value));
    BOOST_REQUIRE(ops::tag(value) == Tag::Symbol);
    auto text = intern_table.get_string(ops::payload(value));
    BOOST_REQUIRE(text.has_value());
    return std::string(*text);
}

std::string output_string_from_port(Heap& heap, LispVal port_val) {
    BOOST_REQUIRE(ops::is_boxed(port_val));
    BOOST_REQUIRE(ops::tag(port_val) == Tag::HeapObject);
    auto* port_obj = heap.try_get_as<ObjectKind::Port, types::PortObject>(ops::payload(port_val));
    BOOST_REQUIRE(port_obj != nullptr);
    auto concrete = std::dynamic_pointer_cast<StringPort>(port_obj->port);
    BOOST_REQUIRE(concrete != nullptr);
    return concrete->get_string();
}

bool is_log_kind(Heap& heap, LispVal value, ObjectKind kind) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) return false;
    HeapEntry entry;
    if (!heap.try_get(ops::payload(value), entry)) return false;
    return entry.header.kind == kind;
}

struct LogFixture {
    Heap heap;
    InternTable intern_table;
    vm::VM vm;
    BuiltinEnvironment env;

    LogFixture()
        : heap(1ull << 22),
          intern_table(),
          vm(heap, intern_table) {
        eta::log::register_log_primitives(env, heap, intern_table, &vm);
    }

    ~LogFixture() {
        spdlog::shutdown();
        eta::log::global_log_state().clear();
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(log_primitives_tests, LogFixture)

BOOST_AUTO_TEST_CASE(registers_expected_builtins) {
    for (const char* name : {
             "%log-make-stdout-sink",
             "%log-make-file-sink",
             "%log-make-rotating-sink",
             "%log-make-daily-sink",
             "%log-make-port-sink",
             "%log-make-current-error-sink",
             "%log-make-logger",
             "%log-get-logger",
             "%log-default-logger",
             "%log-set-default!",
             "%log-set-level!",
             "%log-level",
             "%log-set-global-level!",
             "%log-set-pattern!",
             "%log-set-formatter!",
             "%log-flush!",
             "%log-flush-on!",
             "%log-emit",
             "%log-shutdown!",
         }) {
        BOOST_TEST(env.lookup(name).has_value());
    }
}

BOOST_AUTO_TEST_CASE(sink_constructors_return_log_sink_objects) {
    const auto stdout_symbol = make_sym(intern_table, "stdout");
    const auto stderr_symbol = make_sym(intern_table, "stderr");

    auto stdout_sink = call_builtin(env, "%log-make-stdout-sink", {True, stdout_symbol});
    auto stderr_sink = call_builtin(env, "%log-make-stdout-sink", {False, stderr_symbol});
    BOOST_REQUIRE(stdout_sink.has_value());
    BOOST_REQUIRE(stderr_sink.has_value());
    BOOST_TEST(is_log_kind(heap, *stdout_sink, ObjectKind::LogSink));
    BOOST_TEST(is_log_kind(heap, *stderr_sink, ObjectKind::LogSink));

    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path();
    const auto file_path = (base / "eta-log-tests-basic.log").string();
    const auto rot_path = (base / "eta-log-tests-rot.log").string();
    const auto daily_path = (base / "eta-log-tests-daily.log").string();

    auto file_sink = call_builtin(env, "%log-make-file-sink",
        {make_str(heap, intern_table, file_path), True});
    auto rotating_sink = call_builtin(env, "%log-make-rotating-sink",
        {make_str(heap, intern_table, rot_path), make_int(4096), make_int(2)});
    auto daily_sink = call_builtin(env, "%log-make-daily-sink",
        {make_str(heap, intern_table, daily_path), make_int(0), make_int(0), make_int(2)});

    BOOST_REQUIRE(file_sink.has_value());
    BOOST_REQUIRE(rotating_sink.has_value());
    BOOST_REQUIRE(daily_sink.has_value());
    BOOST_TEST(is_log_kind(heap, *file_sink, ObjectKind::LogSink));
    BOOST_TEST(is_log_kind(heap, *rotating_sink, ObjectKind::LogSink));
    BOOST_TEST(is_log_kind(heap, *daily_sink, ObjectKind::LogSink));
}

BOOST_AUTO_TEST_CASE(human_and_json_emit_write_to_current_error_port) {
    auto out_port = make_port(heap, std::make_shared<StringPort>(StringPort::Mode::Output));
    BOOST_REQUIRE(out_port.has_value());
    vm.set_current_error_port(*out_port);

    auto sink = call_builtin(env, "%log-make-current-error-sink", {});
    BOOST_REQUIRE(sink.has_value());
    auto sinks = make_list(heap, {*sink});

    auto logger = call_builtin(env, "%log-make-logger",
        {make_str(heap, intern_table, "log.test"), sinks});
    BOOST_REQUIRE(logger.has_value());
    BOOST_TEST(is_log_kind(heap, *logger, ObjectKind::LogLogger));

    auto pattern = call_builtin(env, "%log-set-pattern!",
        {*logger, make_str(heap, intern_table, "%v")});
    BOOST_REQUIRE(pattern.has_value());

    auto payload = make_list(heap, {
        make_pair(heap, make_sym(intern_table, "code"), make_int(7)),
    });
    auto emit_human = call_builtin(env, "%log-emit",
        {*logger, make_sym(intern_table, "info"), make_str(heap, intern_table, "human-msg"), payload});
    BOOST_REQUIRE(emit_human.has_value());

    auto set_json = call_builtin(env, "%log-set-formatter!",
        {*logger, make_sym(intern_table, "json")});
    BOOST_REQUIRE(set_json.has_value());
    auto emit_json = call_builtin(env, "%log-emit",
        {*logger, make_sym(intern_table, "info"), make_str(heap, intern_table, "json-msg"), payload});
    BOOST_REQUIRE(emit_json.has_value());

    auto flush = call_builtin(env, "%log-flush!", {*logger});
    BOOST_REQUIRE(flush.has_value());

    auto out = output_string_from_port(heap, *out_port);
    BOOST_TEST(out.find("human-msg") != std::string::npos);
    BOOST_TEST(out.find("code=7") != std::string::npos);
    BOOST_TEST(out.find("\"msg\":\"json-msg\"") != std::string::npos);
    BOOST_TEST(out.find("\"code\":7") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(set_level_filters_messages) {
    auto out_port = make_port(heap, std::make_shared<StringPort>(StringPort::Mode::Output));
    BOOST_REQUIRE(out_port.has_value());
    vm.set_current_error_port(*out_port);

    auto sink = call_builtin(env, "%log-make-current-error-sink", {});
    BOOST_REQUIRE(sink.has_value());
    auto logger = call_builtin(env, "%log-make-logger",
        {make_str(heap, intern_table, "log.filter"), make_list(heap, {*sink})});
    BOOST_REQUIRE(logger.has_value());

    BOOST_REQUIRE(call_builtin(env, "%log-set-pattern!",
        {*logger, make_str(heap, intern_table, "%v")}).has_value());
    BOOST_REQUIRE(call_builtin(env, "%log-set-level!",
        {*logger, make_sym(intern_table, "error")}).has_value());

    BOOST_REQUIRE(call_builtin(env, "%log-emit",
        {*logger, make_sym(intern_table, "info"), make_str(heap, intern_table, "drop-me"), Nil}).has_value());
    BOOST_REQUIRE(call_builtin(env, "%log-emit",
        {*logger, make_sym(intern_table, "error"), make_str(heap, intern_table, "keep-me"), Nil}).has_value());
    BOOST_REQUIRE(call_builtin(env, "%log-flush!", {*logger}).has_value());

    auto out = output_string_from_port(heap, *out_port);
    BOOST_TEST(out.find("keep-me") != std::string::npos);
    BOOST_TEST(out.find("drop-me") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(default_logger_is_vm_scoped_and_resolves_error_port_at_emit_time) {
    auto vm1_out = make_port(heap, std::make_shared<StringPort>(StringPort::Mode::Output));
    BOOST_REQUIRE(vm1_out.has_value());

    auto default_1 = call_builtin(env, "%log-default-logger", {});
    BOOST_REQUIRE(default_1.has_value());
    BOOST_TEST(is_log_kind(heap, *default_1, ObjectKind::LogLogger));

    BOOST_REQUIRE(call_builtin(env, "%log-set-pattern!",
        {*default_1, make_str(heap, intern_table, "%v")}).has_value());
    vm.set_current_error_port(*vm1_out);
    BOOST_REQUIRE(call_builtin(env, "%log-emit",
        {*default_1, make_sym(intern_table, "info"), make_str(heap, intern_table, "vm1"), Nil}).has_value());
    BOOST_REQUIRE(call_builtin(env, "%log-flush!", {*default_1}).has_value());
    BOOST_TEST(output_string_from_port(heap, *vm1_out).find("vm1") != std::string::npos);

    Heap heap2(1ull << 22);
    InternTable intern2;
    vm::VM vm2(heap2, intern2);
    BuiltinEnvironment env2;
    eta::log::register_log_primitives(env2, heap2, intern2, &vm2);

    auto default_2 = call_builtin(env2, "%log-default-logger", {});
    BOOST_REQUIRE(default_2.has_value());
    BOOST_TEST(is_log_kind(heap2, *default_2, ObjectKind::LogLogger));

    BOOST_REQUIRE(call_builtin(env, "%log-set-level!",
        {*default_1, make_sym(intern_table, "debug")}).has_value());
    auto level_1 = call_builtin(env, "%log-level", {*default_1});
    auto level_2 = call_builtin(env2, "%log-level", {*default_2});
    BOOST_REQUIRE(level_1.has_value());
    BOOST_REQUIRE(level_2.has_value());
    BOOST_TEST(decode_symbol(intern_table, *level_1) == "debug");
    BOOST_TEST(decode_symbol(intern2, *level_2) == "info");
}

BOOST_AUTO_TEST_CASE(shutdown_is_idempotent) {
    auto first = call_builtin(env, "%log-shutdown!", {});
    auto second = call_builtin(env, "%log-shutdown!", {});
    BOOST_REQUIRE(first.has_value());
    BOOST_REQUIRE(second.has_value());
    BOOST_TEST(*first == Nil);
    BOOST_TEST(*second == Nil);
}

BOOST_AUTO_TEST_SUITE_END()
