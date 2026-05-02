#include <boost/test/unit_test.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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

std::vector<std::string> decode_string_vector(Heap& heap, InternTable& intern_table, LispVal value) {
    BOOST_REQUIRE(ops::is_boxed(value));
    BOOST_REQUIRE(ops::tag(value) == Tag::HeapObject);
    auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(value));
    BOOST_REQUIRE(vec != nullptr);

    std::vector<std::string> out;
    out.reserve(vec->elements.size());
    for (auto v : vec->elements) {
        out.push_back(decode_string(intern_table, v));
    }
    return out;
}

LispVal alist_lookup(Heap& heap, LispVal alist, LispVal key) {
    LispVal cur = alist;
    while (cur != Nil) {
        BOOST_REQUIRE(ops::is_boxed(cur));
        BOOST_REQUIRE(ops::tag(cur) == Tag::HeapObject);
        auto* row = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        BOOST_REQUIRE(row != nullptr);
        BOOST_REQUIRE(ops::is_boxed(row->car));
        BOOST_REQUIRE(ops::tag(row->car) == Tag::HeapObject);

        auto* kv = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(row->car));
        BOOST_REQUIRE(kv != nullptr);
        if (kv->car == key) return kv->cdr;
        cur = row->cdr;
    }
    return False;
}

struct TempCsvFile {
    std::filesystem::path path;

    TempCsvFile() {
        const auto stamp =
            std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
            / ("eta_csv_writer_tests_" + std::to_string(stamp) + ".csv");
    }

    ~TempCsvFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    BOOST_REQUIRE(in.is_open());
    std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return out;
}

} // namespace

BOOST_AUTO_TEST_SUITE(csv_writer_tests)

BOOST_AUTO_TEST_CASE(registers_expected_csv_writer_builtins) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    BOOST_TEST(env.lookup("%csv-open-writer").has_value());
    BOOST_TEST(env.lookup("%csv-write-row").has_value());
    BOOST_TEST(env.lookup("%csv-write-record").has_value());
    BOOST_TEST(env.lookup("%csv-flush").has_value());
    BOOST_TEST(env.lookup("%csv-writer?").has_value());
}

BOOST_AUTO_TEST_CASE(round_trip_write_row_and_write_record) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    TempCsvFile file;
    LispVal writer_opts = alist_from_pairs(heap, {
        {symbol(intern_table, "column-names"),
         list_from_values(heap, {
             stringv(heap, intern_table, "name"),
             stringv(heap, intern_table, "note"),
             stringv(heap, intern_table, "n")
         })}
    });

    auto writer = call_builtin(env, "%csv-open-writer",
        {stringv(heap, intern_table, file.path.string()), writer_opts});
    BOOST_REQUIRE(writer.has_value());

    auto wrote_row = call_builtin(env, "%csv-write-row", {
        *writer,
        list_from_values(heap, {
            stringv(heap, intern_table, "Alice, A."),
            stringv(heap, intern_table, "He said \"hello\""),
            ops::encode<int64_t>(10).value_or(Nil)
        })
    });
    BOOST_REQUIRE(wrote_row.has_value());

    LispVal record = alist_from_pairs(heap, {
        {symbol(intern_table, "name"), stringv(heap, intern_table, "Bob")},
        {symbol(intern_table, "note"), stringv(heap, intern_table, "line1\nline2")},
        {symbol(intern_table, "n"), ops::encode<int64_t>(20).value_or(Nil)}
    });
    LispVal columns = list_from_values(heap, {
        symbol(intern_table, "name"),
        symbol(intern_table, "note"),
        symbol(intern_table, "n")
    });
    auto wrote_record = call_builtin(env, "%csv-write-record", {*writer, record, columns});
    BOOST_REQUIRE(wrote_record.has_value());

    auto closed_writer = call_builtin(env, "%csv-close", {*writer});
    BOOST_REQUIRE(closed_writer.has_value());

    auto reader = call_builtin(env, "%csv-open-reader", {
        stringv(heap, intern_table, file.path.string()),
        Nil
    });
    BOOST_REQUIRE(reader.has_value());

    auto row1 = call_builtin(env, "%csv-read-row", {*reader});
    auto row2 = call_builtin(env, "%csv-read-row", {*reader});
    BOOST_REQUIRE(row1.has_value());
    BOOST_REQUIRE(row2.has_value());
    BOOST_TEST(decode_string_vector(heap, intern_table, *row1)
        == std::vector<std::string>({"Alice, A.", "He said \"hello\"", "10"}));
    BOOST_TEST(decode_string_vector(heap, intern_table, *row2)
        == std::vector<std::string>({"Bob", "line1\nline2", "20"}));

    auto rec = call_builtin(env, "%csv-read-record", {*reader});
    BOOST_REQUIRE(rec.has_value());
    BOOST_TEST(*rec == False);

    auto closed_reader = call_builtin(env, "%csv-close", {*reader});
    BOOST_REQUIRE(closed_reader.has_value());
}

BOOST_AUTO_TEST_CASE(flush_persists_mid_stream_writes) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    TempCsvFile file;
    auto writer = call_builtin(env, "%csv-open-writer", {
        stringv(heap, intern_table, file.path.string()),
        alist_from_pairs(heap, {{symbol(intern_table, "header"), False}})
    });
    BOOST_REQUIRE(writer.has_value());

    auto wrote_row = call_builtin(env, "%csv-write-row", {
        *writer,
        list_from_values(heap, {
            stringv(heap, intern_table, "a"),
            ops::encode<int64_t>(1).value_or(Nil)
        })
    });
    BOOST_REQUIRE(wrote_row.has_value());

    auto flushed = call_builtin(env, "%csv-flush", {*writer});
    BOOST_REQUIRE(flushed.has_value());

    BOOST_TEST(read_file(file.path) == std::string("a,1\n"));

    auto closed_writer = call_builtin(env, "%csv-close", {*writer});
    BOOST_REQUIRE(closed_writer.has_value());
}

BOOST_AUTO_TEST_CASE(quote_policy_controls_output) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    TempCsvFile all_file;
    auto writer_all = call_builtin(env, "%csv-open-writer", {
        stringv(heap, intern_table, all_file.path.string()),
        alist_from_pairs(heap, {
            {symbol(intern_table, "header"), False},
            {symbol(intern_table, "quote-policy"), symbol(intern_table, "all")}
        })
    });
    BOOST_REQUIRE(writer_all.has_value());

    auto wrote_all = call_builtin(env, "%csv-write-row", {
        *writer_all,
        list_from_values(heap, {ops::encode<int64_t>(1).value_or(Nil), stringv(heap, intern_table, "alpha")})
    });
    BOOST_REQUIRE(wrote_all.has_value());
    auto closed_all = call_builtin(env, "%csv-close", {*writer_all});
    BOOST_REQUIRE(closed_all.has_value());
    BOOST_TEST(read_file(all_file.path) == std::string("\"1\",\"alpha\"\n"));

    TempCsvFile nn_file;
    auto writer_nn = call_builtin(env, "%csv-open-writer", {
        stringv(heap, intern_table, nn_file.path.string()),
        alist_from_pairs(heap, {
            {symbol(intern_table, "header"), False},
            {symbol(intern_table, "quote-policy"), symbol(intern_table, "non-numeric")}
        })
    });
    BOOST_REQUIRE(writer_nn.has_value());

    auto wrote_nn = call_builtin(env, "%csv-write-row", {
        *writer_nn,
        list_from_values(heap, {ops::encode<int64_t>(1).value_or(Nil), stringv(heap, intern_table, "alpha")})
    });
    BOOST_REQUIRE(wrote_nn.has_value());
    auto closed_nn = call_builtin(env, "%csv-close", {*writer_nn});
    BOOST_REQUIRE(closed_nn.has_value());
    BOOST_TEST(read_file(nn_file.path) == std::string("1,\"alpha\"\n"));
}

BOOST_AUTO_TEST_SUITE_END()
