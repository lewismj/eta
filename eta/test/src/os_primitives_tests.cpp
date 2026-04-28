#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <eta/runtime/builtin_env.h>
#include <eta/runtime/error.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/numeric_value.h>
#include <eta/runtime/os_primitives.h>
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

void expect_type_error(const std::expected<LispVal, RuntimeError>& result) {
    BOOST_REQUIRE(!result.has_value());
    auto* vm_error = std::get_if<VMError>(&result.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_TEST(static_cast<int>(vm_error->code) ==
               static_cast<int>(RuntimeErrorCode::TypeError));
}

std::string decode_string(InternTable& intern_table, LispVal value) {
    auto sv = StringView::try_from(value, intern_table);
    BOOST_REQUIRE(sv.has_value());
    return std::string(sv->view());
}

std::vector<std::string> decode_string_list(Heap& heap, InternTable& intern_table, LispVal list) {
    std::vector<std::string> out;
    LispVal cur = list;
    while (cur != Nil) {
        BOOST_REQUIRE(ops::is_boxed(cur));
        BOOST_REQUIRE(ops::tag(cur) == Tag::HeapObject);
        auto* cell = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        BOOST_REQUIRE(cell != nullptr);
        out.push_back(decode_string(intern_table, cell->car));
        cur = cell->cdr;
    }
    return out;
}

std::unordered_map<std::string, std::string>
decode_string_alist(Heap& heap, InternTable& intern_table, LispVal list) {
    std::unordered_map<std::string, std::string> out;
    LispVal cur = list;
    while (cur != Nil) {
        BOOST_REQUIRE(ops::is_boxed(cur));
        BOOST_REQUIRE(ops::tag(cur) == Tag::HeapObject);
        auto* outer = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        BOOST_REQUIRE(outer != nullptr);

        BOOST_REQUIRE(ops::is_boxed(outer->car));
        BOOST_REQUIRE(ops::tag(outer->car) == Tag::HeapObject);
        auto* inner = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(outer->car));
        BOOST_REQUIRE(inner != nullptr);

        out.emplace(
            decode_string(intern_table, inner->car),
            decode_string(intern_table, inner->cdr));
        cur = outer->cdr;
    }
    return out;
}

fs::path unique_test_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / ("eta-os-prims-test-" + std::to_string(stamp));
}

bool path_equivalent(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::exists(a, ec) && !ec && fs::exists(b, ec) && !ec) {
        if (fs::equivalent(a, b, ec) && !ec) return true;
    }
    return a.lexically_normal().make_preferred() == b.lexically_normal().make_preferred();
}

struct OsPrimitiveFixture {
    Heap heap;
    InternTable intern_table;
    vm::VM vm;
    BuiltinEnvironment env;
    fs::path original_cwd;
    std::vector<fs::path> cleanup_paths;

    OsPrimitiveFixture()
        : heap(1ull << 22),
          intern_table(),
          vm(heap, intern_table) {
        std::error_code ec;
        original_cwd = fs::current_path(ec);
        const std::array<std::string, 2> cli_args = {"alpha", "beta"};
        register_os_primitives(env, heap, intern_table, vm, std::span<const std::string>(cli_args));
    }

    ~OsPrimitiveFixture() {
        std::error_code ec;
        if (!original_cwd.empty()) {
            fs::current_path(original_cwd, ec);
        }
        for (const auto& p : cleanup_paths) {
            fs::remove_all(p, ec);
        }
    }

    void track_for_cleanup(const fs::path& path) {
        cleanup_paths.push_back(path);
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(os_primitives_tests, OsPrimitiveFixture)

BOOST_AUTO_TEST_CASE(registers_expected_builtins) {
    BOOST_TEST(env.lookup("getenv").has_value());
    BOOST_TEST(env.lookup("setenv!").has_value());
    BOOST_TEST(env.lookup("unsetenv!").has_value());
    BOOST_TEST(env.lookup("environment-variables").has_value());
    BOOST_TEST(env.lookup("command-line-arguments").has_value());
    BOOST_TEST(env.lookup("exit").has_value());
    BOOST_TEST(env.lookup("current-directory").has_value());
    BOOST_TEST(env.lookup("change-directory!").has_value());
    BOOST_TEST(env.lookup("file-exists?").has_value());
    BOOST_TEST(env.lookup("directory?").has_value());
    BOOST_TEST(env.lookup("delete-file").has_value());
    BOOST_TEST(env.lookup("make-directory").has_value());
    BOOST_TEST(env.lookup("list-directory").has_value());
    BOOST_TEST(env.lookup("path-join").has_value());
    BOOST_TEST(env.lookup("path-split").has_value());
    BOOST_TEST(env.lookup("path-normalize").has_value());
    BOOST_TEST(env.lookup("temp-file").has_value());
    BOOST_TEST(env.lookup("temp-directory").has_value());
    BOOST_TEST(env.lookup("file-modification-time").has_value());
    BOOST_TEST(env.lookup("file-size").has_value());
}

BOOST_AUTO_TEST_CASE(environment_and_arguments_primitives_work) {
    const auto key_val = make_string(heap, intern_table, "ETA_OS_PRIMITIVES_TEST_VAR");
    const auto value_val = make_string(heap, intern_table, "eta-os-value");
    BOOST_REQUIRE(key_val.has_value());
    BOOST_REQUIRE(value_val.has_value());

    auto unset_res = call_builtin(env, "unsetenv!", {*key_val});
    BOOST_REQUIRE(unset_res.has_value());

    auto missing = call_builtin(env, "getenv", {*key_val});
    BOOST_REQUIRE(missing.has_value());
    BOOST_TEST(*missing == False);

    auto set_res = call_builtin(env, "setenv!", {*key_val, *value_val});
    BOOST_REQUIRE(set_res.has_value());

    auto got = call_builtin(env, "getenv", {*key_val});
    BOOST_REQUIRE(got.has_value());
    BOOST_TEST(decode_string(intern_table, *got) == "eta-os-value");

    auto env_vars = call_builtin(env, "environment-variables", {});
    BOOST_REQUIRE(env_vars.has_value());
    auto env_map = decode_string_alist(heap, intern_table, *env_vars);
    BOOST_TEST(env_map.contains("ETA_OS_PRIMITIVES_TEST_VAR"));
    BOOST_TEST(env_map["ETA_OS_PRIMITIVES_TEST_VAR"] == "eta-os-value");

    auto args = call_builtin(env, "command-line-arguments", {});
    BOOST_REQUIRE(args.has_value());
    auto arg_values = decode_string_list(heap, intern_table, *args);
    BOOST_REQUIRE(arg_values.size() == 2u);
    BOOST_TEST(arg_values[0] == "alpha");
    BOOST_TEST(arg_values[1] == "beta");

    auto final_unset = call_builtin(env, "unsetenv!", {*key_val});
    BOOST_REQUIRE(final_unset.has_value());
}

BOOST_AUTO_TEST_CASE(directory_and_path_primitives_work) {
    auto cwd = call_builtin(env, "current-directory", {});
    BOOST_REQUIRE(cwd.has_value());
    BOOST_TEST(path_equivalent(fs::path(decode_string(intern_table, *cwd)), fs::current_path()));

    const fs::path temp_dir = unique_test_dir();
    fs::create_directory(temp_dir);
    track_for_cleanup(temp_dir);

    auto temp_dir_val = make_string(heap, intern_table, temp_dir.string());
    BOOST_REQUIRE(temp_dir_val.has_value());
    auto chdir = call_builtin(env, "change-directory!", {*temp_dir_val});
    BOOST_REQUIRE(chdir.has_value());

    auto cwd_after = call_builtin(env, "current-directory", {});
    BOOST_REQUIRE(cwd_after.has_value());
    BOOST_TEST(path_equivalent(fs::path(decode_string(intern_table, *cwd_after)), temp_dir));

    auto original_val = make_string(heap, intern_table, original_cwd.string());
    BOOST_REQUIRE(original_val.has_value());
    auto chdir_back = call_builtin(env, "change-directory!", {*original_val});
    BOOST_REQUIRE(chdir_back.has_value());
    BOOST_TEST(path_equivalent(fs::current_path(), original_cwd));

    auto seg_a = make_string(heap, intern_table, "a");
    auto seg_b = make_string(heap, intern_table, "b");
    auto seg_c = make_string(heap, intern_table, "c.txt");
    BOOST_REQUIRE(seg_a.has_value());
    BOOST_REQUIRE(seg_b.has_value());
    BOOST_REQUIRE(seg_c.has_value());

    auto joined = call_builtin(env, "path-join", {*seg_a, *seg_b, *seg_c});
    BOOST_REQUIRE(joined.has_value());
    BOOST_TEST(fs::path(decode_string(intern_table, *joined)).filename().string() == "c.txt");

    auto split = call_builtin(env, "path-split", {*joined});
    BOOST_REQUIRE(split.has_value());
    auto split_parts = decode_string_list(heap, intern_table, *split);
    BOOST_REQUIRE(!split_parts.empty());
    BOOST_TEST(split_parts.back() == "c.txt");

    auto to_normalize = make_string(heap, intern_table, "a/./b/../c");
    BOOST_REQUIRE(to_normalize.has_value());
    auto normalized = call_builtin(env, "path-normalize", {*to_normalize});
    BOOST_REQUIRE(normalized.has_value());
    BOOST_TEST(fs::path(decode_string(intern_table, *normalized)).lexically_normal() ==
               fs::path("a/c").lexically_normal());
}

BOOST_AUTO_TEST_CASE(filesystem_metadata_primitives_work) {
    auto temp_dir_res = call_builtin(env, "temp-directory", {});
    BOOST_REQUIRE(temp_dir_res.has_value());
    const fs::path temp_dir = decode_string(intern_table, *temp_dir_res);
    track_for_cleanup(temp_dir);

    auto temp_dir_val = make_string(heap, intern_table, temp_dir.string());
    BOOST_REQUIRE(temp_dir_val.has_value());

    auto dir_pred = call_builtin(env, "directory?", {*temp_dir_val});
    BOOST_REQUIRE(dir_pred.has_value());
    BOOST_TEST(*dir_pred == True);

    auto child_name = make_string(heap, intern_table, "child");
    BOOST_REQUIRE(child_name.has_value());
    auto child_dir = call_builtin(env, "path-join", {*temp_dir_val, *child_name});
    BOOST_REQUIRE(child_dir.has_value());
    auto mk_child = call_builtin(env, "make-directory", {*child_dir});
    BOOST_REQUIRE(mk_child.has_value());

    auto entries = call_builtin(env, "list-directory", {*temp_dir_val});
    BOOST_REQUIRE(entries.has_value());
    auto entry_names = decode_string_list(heap, intern_table, *entries);
    const bool has_child = std::find(entry_names.begin(), entry_names.end(), "child") != entry_names.end();
    BOOST_TEST(has_child);

    auto temp_file_res = call_builtin(env, "temp-file", {});
    BOOST_REQUIRE(temp_file_res.has_value());
    const fs::path temp_file = decode_string(intern_table, *temp_file_res);
    track_for_cleanup(temp_file);

    auto temp_file_val = make_string(heap, intern_table, temp_file.string());
    BOOST_REQUIRE(temp_file_val.has_value());

    auto exists_res = call_builtin(env, "file-exists?", {*temp_file_val});
    BOOST_REQUIRE(exists_res.has_value());
    BOOST_TEST(*exists_res == True);

    auto size_res = call_builtin(env, "file-size", {*temp_file_val});
    BOOST_REQUIRE(size_res.has_value());
    auto size_num = classify_numeric(*size_res, heap);
    BOOST_REQUIRE(size_num.is_valid());
    BOOST_TEST(size_num.int_val == 0);

    {
        std::ofstream out(temp_file, std::ios::binary | std::ios::trunc);
        out << "eta";
    }

    auto mtime_res = call_builtin(env, "file-modification-time", {*temp_file_val});
    BOOST_REQUIRE(mtime_res.has_value());
    auto mtime_num = classify_numeric(*mtime_res, heap);
    BOOST_REQUIRE(mtime_num.is_valid());
    BOOST_TEST(mtime_num.is_fixnum());

    auto delete_res = call_builtin(env, "delete-file", {*temp_file_val});
    BOOST_REQUIRE(delete_res.has_value());

    auto exists_after = call_builtin(env, "file-exists?", {*temp_file_val});
    BOOST_REQUIRE(exists_after.has_value());
    BOOST_TEST(*exists_after == False);
}

BOOST_AUTO_TEST_CASE(exit_rejects_non_numeric_non_boolean_status) {
    auto bad = make_string(heap, intern_table, "bad");
    BOOST_REQUIRE(bad.has_value());
    expect_type_error(call_builtin(env, "exit", {*bad}));
}

BOOST_AUTO_TEST_SUITE_END()
