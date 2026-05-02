#include <boost/test/unit_test.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <eta/runtime/builtin_env.h>
#include <eta/runtime/core_primitives.h>
#include <eta/runtime/error.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/string_view.h>
#include <eta/runtime/types/types.h>

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::error;

namespace {

std::expected<LispVal, RuntimeError> call_builtin(
    BuiltinEnvironment& env,
    const std::string& name,
    const std::vector<LispVal>& args) {
    auto idx = env.lookup(name);
    if (!idx) {
        return std::unexpected(RuntimeError{
            VMError{RuntimeErrorCode::InternalError, "missing builtin: " + name}});
    }
    return env.specs()[*idx].func(args);
}

LispVal symbol(InternTable& intern_table, std::string_view text) {
    auto sym = make_symbol(intern_table, std::string(text));
    BOOST_REQUIRE(sym.has_value());
    return *sym;
}

LispVal stringv(Heap& heap, InternTable& intern_table, std::string_view text) {
    auto str = make_string(heap, intern_table, std::string(text));
    BOOST_REQUIRE(str.has_value());
    return *str;
}

LispVal list_from_values(Heap& heap, const std::vector<LispVal>& values) {
    LispVal out = Nil;
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        auto cell = make_cons(heap, *it, out);
        BOOST_REQUIRE(cell.has_value());
        out = *cell;
    }
    return out;
}

LispVal alist_from_pairs(Heap& heap, const std::vector<std::pair<LispVal, LispVal>>& pairs) {
    LispVal out = Nil;
    for (auto it = pairs.rbegin(); it != pairs.rend(); ++it) {
        auto pair = make_cons(heap, it->first, it->second);
        BOOST_REQUIRE(pair.has_value());
        auto row = make_cons(heap, *pair, out);
        BOOST_REQUIRE(row.has_value());
        out = *row;
    }
    return out;
}

std::string decode_string(InternTable& intern_table, LispVal value) {
    auto sv = StringView::try_from(value, intern_table);
    BOOST_REQUIRE(sv.has_value());
    return std::string(sv->view());
}

struct TempCsvFile {
    std::filesystem::path path;

    TempCsvFile() {
        const auto stamp =
            std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
            / ("eta_csv_fact_table_tests_" + std::to_string(stamp) + ".csv");
    }

    ~TempCsvFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

void write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    BOOST_REQUIRE(out.is_open());
    out << text;
    BOOST_REQUIRE(out.good());
}

types::FactTable* require_fact_table_ptr(Heap& heap, LispVal value) {
    BOOST_REQUIRE(ops::is_boxed(value));
    BOOST_REQUIRE(ops::tag(value) == Tag::HeapObject);
    auto* table = heap.try_get_as<ObjectKind::FactTable, types::FactTable>(ops::payload(value));
    BOOST_REQUIRE(table != nullptr);
    return table;
}

} // namespace

BOOST_AUTO_TEST_SUITE(csv_fact_table_tests)

BOOST_AUTO_TEST_CASE(registers_fact_table_csv_builtins) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    BOOST_TEST(env.lookup("%fact-table-load-csv").has_value());
    BOOST_TEST(env.lookup("%fact-table-save-csv").has_value());
}

BOOST_AUTO_TEST_CASE(load_csv_infers_numeric_cells) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    TempCsvFile file;
    write_file(file.path, "sector,beta,ret\ntech,1.2,2.3\nenergy,0.9,1.1\n");

    auto table_val = call_builtin(env, "%fact-table-load-csv", {
        stringv(heap, intern_table, file.path.string()),
        Nil
    });
    BOOST_REQUIRE(table_val.has_value());

    auto* table = require_fact_table_ptr(heap, *table_val);
    BOOST_TEST(table->active_row_count() == 2u);
    BOOST_TEST(decode_string(intern_table, table->get_cell(0, 0)) == "tech");

    auto beta = classify_numeric(table->get_cell(0, 1), heap);
    auto ret = classify_numeric(table->get_cell(0, 2), heap);
    BOOST_TEST(beta.is_flonum());
    BOOST_TEST(ret.is_flonum());
    BOOST_TEST(beta.float_val == 1.2, boost::test_tools::tolerance(1e-12));
    BOOST_TEST(ret.float_val == 2.3, boost::test_tools::tolerance(1e-12));
}

BOOST_AUTO_TEST_CASE(load_csv_without_inference_keeps_numbers_as_strings) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    TempCsvFile file;
    write_file(file.path, "name,qty\nalpha,10\nbeta,20\n");

    auto opts = alist_from_pairs(heap, {
        {symbol(intern_table, "infer-types?"), False}
    });
    auto table_val = call_builtin(env, "%fact-table-load-csv", {
        stringv(heap, intern_table, file.path.string()),
        opts
    });
    BOOST_REQUIRE(table_val.has_value());

    auto* table = require_fact_table_ptr(heap, *table_val);
    BOOST_TEST(table->active_row_count() == 2u);
    BOOST_TEST(decode_string(intern_table, table->get_cell(0, 1)) == "10");
    BOOST_TEST(decode_string(intern_table, table->get_cell(1, 1)) == "20");
}

BOOST_AUTO_TEST_CASE(save_csv_round_trips_live_rows) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto table_cols = list_from_values(heap, {
        symbol(intern_table, "name"),
        symbol(intern_table, "score")
    });
    auto table_val = call_builtin(env, "%make-fact-table", {table_cols});
    BOOST_REQUIRE(table_val.has_value());

    auto row1 = list_from_values(heap, {stringv(heap, intern_table, "alice"), ops::encode<int64_t>(10).value_or(Nil)});
    auto row2 = list_from_values(heap, {stringv(heap, intern_table, "bob"), ops::encode<int64_t>(20).value_or(Nil)});
    auto ins1 = call_builtin(env, "%fact-table-insert!", {*table_val, row1});
    auto ins2 = call_builtin(env, "%fact-table-insert!", {*table_val, row2});
    BOOST_REQUIRE(ins1.has_value());
    BOOST_REQUIRE(ins2.has_value());

    auto del = call_builtin(env, "%fact-table-delete-row!", {*table_val, ops::encode<int64_t>(0).value_or(Nil)});
    BOOST_REQUIRE(del.has_value());

    TempCsvFile file;
    auto saved = call_builtin(env, "%fact-table-save-csv", {
        *table_val,
        stringv(heap, intern_table, file.path.string()),
        Nil
    });
    BOOST_REQUIRE(saved.has_value());

    auto loaded_val = call_builtin(env, "%fact-table-load-csv", {
        stringv(heap, intern_table, file.path.string()),
        Nil
    });
    BOOST_REQUIRE(loaded_val.has_value());

    auto* loaded = require_fact_table_ptr(heap, *loaded_val);
    BOOST_TEST(loaded->active_row_count() == 1u);
    BOOST_TEST(decode_string(intern_table, loaded->get_cell(0, 0)) == "bob");
    auto score = classify_numeric(loaded->get_cell(0, 1), heap);
    BOOST_TEST(score.is_fixnum());
    BOOST_TEST(score.int_val == 20);
}

BOOST_AUTO_TEST_SUITE_END()
