#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <eta/runtime/builtin_env.h>
#include <eta/runtime/core_primitives.h>
#include <eta/runtime/error.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/memory/mark_sweep_gc.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/string_view.h>
#include <eta/runtime/types/types.h>

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::memory::gc;
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

LispVal charv(char32_t ch) {
    auto value = ops::encode(ch);
    BOOST_REQUIRE(value.has_value());
    return *value;
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

std::vector<LispVal> decode_list(Heap& heap, LispVal list) {
    std::vector<LispVal> out;
    LispVal cur = list;
    while (cur != Nil) {
        BOOST_REQUIRE(ops::is_boxed(cur));
        BOOST_REQUIRE(ops::tag(cur) == Tag::HeapObject);
        auto* cell = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        BOOST_REQUIRE(cell != nullptr);
        out.push_back(cell->car);
        cur = cell->cdr;
    }
    return out;
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

std::vector<std::string> decode_symbol_vector(Heap& heap, InternTable& intern_table, LispVal value) {
    BOOST_REQUIRE(ops::is_boxed(value));
    BOOST_REQUIRE(ops::tag(value) == Tag::HeapObject);
    auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(value));
    BOOST_REQUIRE(vec != nullptr);

    std::vector<std::string> out;
    out.reserve(vec->elements.size());
    for (auto v : vec->elements) {
        BOOST_REQUIRE(ops::is_boxed(v));
        BOOST_REQUIRE(ops::tag(v) == Tag::Symbol);
        auto text = intern_table.get_string(ops::payload(v));
        BOOST_REQUIRE(text.has_value());
        out.emplace_back(*text);
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

} // namespace

BOOST_AUTO_TEST_SUITE(csv_reader_tests)

BOOST_AUTO_TEST_CASE(registers_expected_csv_builtins) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    BOOST_TEST(env.lookup("%csv-open-reader").has_value());
    BOOST_TEST(env.lookup("%csv-reader-from-string").has_value());
    BOOST_TEST(env.lookup("%csv-columns").has_value());
    BOOST_TEST(env.lookup("%csv-read-row").has_value());
    BOOST_TEST(env.lookup("%csv-read-record").has_value());
    BOOST_TEST(env.lookup("%csv-read-typed-row").has_value());
    BOOST_TEST(env.lookup("%csv-close").has_value());
    BOOST_TEST(env.lookup("%csv-reader?").has_value());
}

BOOST_AUTO_TEST_CASE(reads_rfc4180_quoted_fields) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    const std::string input =
        "name,quote,note\n"
        "\"Alice, A.\",\"He said \"\"hello\"\"\",\"line1\nline2\"\n";

    auto reader = call_builtin(env, "%csv-reader-from-string",
        {stringv(heap, intern_table, input), Nil});
    BOOST_REQUIRE(reader.has_value());

    auto columns = call_builtin(env, "%csv-columns", {*reader});
    BOOST_REQUIRE(columns.has_value());
    BOOST_TEST(decode_symbol_vector(heap, intern_table, *columns)
        == std::vector<std::string>({"name", "quote", "note"}));

    auto row = call_builtin(env, "%csv-read-row", {*reader});
    BOOST_REQUIRE(row.has_value());
    BOOST_TEST(decode_string_vector(heap, intern_table, *row)
        == std::vector<std::string>({"Alice, A.", "He said \"hello\"", "line1\nline2"}));

    auto eof = call_builtin(env, "%csv-read-row", {*reader});
    BOOST_REQUIRE(eof.has_value());
    BOOST_TEST(*eof == False);
}

BOOST_AUTO_TEST_CASE(handles_crlf_lf_and_utf8_bom) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    const std::string input =
        "\xEF\xBB\xBF"
        "c1,c2\r\n"
        "a,1\n"
        "b,2\r\n";

    auto reader = call_builtin(env, "%csv-reader-from-string",
        {stringv(heap, intern_table, input), Nil});
    BOOST_REQUIRE(reader.has_value());

    auto columns = call_builtin(env, "%csv-columns", {*reader});
    BOOST_REQUIRE(columns.has_value());
    BOOST_TEST(decode_symbol_vector(heap, intern_table, *columns)
        == std::vector<std::string>({"c1", "c2"}));

    auto row1 = call_builtin(env, "%csv-read-row", {*reader});
    auto row2 = call_builtin(env, "%csv-read-row", {*reader});
    BOOST_REQUIRE(row1.has_value());
    BOOST_REQUIRE(row2.has_value());
    BOOST_TEST(decode_string_vector(heap, intern_table, *row1)
        == std::vector<std::string>({"a", "1"}));
    BOOST_TEST(decode_string_vector(heap, intern_table, *row2)
        == std::vector<std::string>({"b", "2"}));
}

BOOST_AUTO_TEST_CASE(supports_custom_delimiter_quote_and_comments) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    const std::string input =
        "left;right\n"
        "#skip;row\n"
        "'v;1';'a''b'\n"
        "#ignore;again\n"
        "plain;42\n";

    LispVal opts = alist_from_pairs(heap, {
        {symbol(intern_table, "delimiter"), charv(';')},
        {symbol(intern_table, "quote"), charv('\'')},
        {symbol(intern_table, "comment"), charv('#')}
    });

    auto reader = call_builtin(env, "%csv-reader-from-string",
        {stringv(heap, intern_table, input), opts});
    BOOST_REQUIRE(reader.has_value());

    auto row1 = call_builtin(env, "%csv-read-row", {*reader});
    auto row2 = call_builtin(env, "%csv-read-row", {*reader});
    auto eof = call_builtin(env, "%csv-read-row", {*reader});
    BOOST_REQUIRE(row1.has_value());
    BOOST_REQUIRE(row2.has_value());
    BOOST_REQUIRE(eof.has_value());

    BOOST_TEST(decode_string_vector(heap, intern_table, *row1)
        == std::vector<std::string>({"v;1", "a'b"}));
    BOOST_TEST(decode_string_vector(heap, intern_table, *row2)
        == std::vector<std::string>({"plain", "42"}));
    BOOST_TEST(*eof == False);
}

BOOST_AUTO_TEST_CASE(supports_header_off_with_column_name_override) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    const std::string input = "10,20\n30,40\n";

    LispVal opts = alist_from_pairs(heap, {
        {symbol(intern_table, "header"), False},
        {symbol(intern_table, "column-names"),
         list_from_values(heap, {
             stringv(heap, intern_table, "x"),
             stringv(heap, intern_table, "y")
         })}
    });

    auto reader = call_builtin(env, "%csv-reader-from-string",
        {stringv(heap, intern_table, input), opts});
    BOOST_REQUIRE(reader.has_value());

    auto columns = call_builtin(env, "%csv-columns", {*reader});
    BOOST_REQUIRE(columns.has_value());
    BOOST_TEST(decode_symbol_vector(heap, intern_table, *columns)
        == std::vector<std::string>({"x", "y"}));

    auto rec1 = call_builtin(env, "%csv-read-record", {*reader});
    auto rec2 = call_builtin(env, "%csv-read-record", {*reader});
    BOOST_REQUIRE(rec1.has_value());
    BOOST_REQUIRE(rec2.has_value());

    LispVal sym_x = symbol(intern_table, "x");
    LispVal sym_y = symbol(intern_table, "y");

    BOOST_TEST(decode_string(intern_table, alist_lookup(heap, *rec1, sym_x)) == "10");
    BOOST_TEST(decode_string(intern_table, alist_lookup(heap, *rec1, sym_y)) == "20");
    BOOST_TEST(decode_string(intern_table, alist_lookup(heap, *rec2, sym_x)) == "30");
    BOOST_TEST(decode_string(intern_table, alist_lookup(heap, *rec2, sym_y)) == "40");
}

BOOST_AUTO_TEST_CASE(reads_typed_rows_with_null_tokens) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    const std::string input =
        "x,y,z,w\n"
        "42,3.5,NA,alpha\n"
        ",7,NaN,0\n";

    auto reader = call_builtin(env, "%csv-reader-from-string",
        {stringv(heap, intern_table, input), Nil});
    BOOST_REQUIRE(reader.has_value());

    auto row1v = call_builtin(env, "%csv-read-typed-row", {*reader});
    auto row2v = call_builtin(env, "%csv-read-typed-row", {*reader});
    BOOST_REQUIRE(row1v.has_value());
    BOOST_REQUIRE(row2v.has_value());
    BOOST_REQUIRE(ops::is_boxed(*row1v));
    BOOST_REQUIRE(ops::is_boxed(*row2v));
    BOOST_REQUIRE(ops::tag(*row1v) == Tag::HeapObject);
    BOOST_REQUIRE(ops::tag(*row2v) == Tag::HeapObject);

    auto* row1 = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(*row1v));
    auto* row2 = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(*row2v));
    BOOST_REQUIRE(row1 != nullptr);
    BOOST_REQUIRE(row2 != nullptr);
    BOOST_REQUIRE(row1->elements.size() == 4);
    BOOST_REQUIRE(row2->elements.size() == 4);

    auto n1 = classify_numeric(row1->elements[0], heap);
    auto n2 = classify_numeric(row1->elements[1], heap);
    BOOST_TEST(n1.is_fixnum());
    BOOST_TEST(n1.int_val == 42);
    BOOST_TEST(n2.is_flonum());
    BOOST_TEST(n2.float_val == 3.5, boost::test_tools::tolerance(1e-12));
    BOOST_TEST(row1->elements[2] == Nil);
    BOOST_TEST(decode_string(intern_table, row1->elements[3]) == "alpha");

    BOOST_TEST(row2->elements[0] == Nil);
    auto n3 = classify_numeric(row2->elements[1], heap);
    auto n4 = classify_numeric(row2->elements[3], heap);
    BOOST_TEST(n3.is_fixnum());
    BOOST_TEST(n3.int_val == 7);
    BOOST_TEST(row2->elements[2] == Nil);
    BOOST_TEST(n4.is_fixnum());
    BOOST_TEST(n4.int_val == 0);
}

BOOST_AUTO_TEST_CASE(typed_row_number_parsing_is_locale_independent) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    const std::string input = "x;y\n1,5;2\n";
    LispVal opts = alist_from_pairs(heap, {
        {symbol(intern_table, "delimiter"), charv(';')}
    });

    auto reader = call_builtin(env, "%csv-reader-from-string",
        {stringv(heap, intern_table, input), opts});
    BOOST_REQUIRE(reader.has_value());

    auto rowv = call_builtin(env, "%csv-read-typed-row", {*reader});
    BOOST_REQUIRE(rowv.has_value());
    BOOST_REQUIRE(ops::is_boxed(*rowv));
    BOOST_REQUIRE(ops::tag(*rowv) == Tag::HeapObject);
    auto* row = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(*rowv));
    BOOST_REQUIRE(row != nullptr);
    BOOST_REQUIRE(row->elements.size() == 2);

    BOOST_TEST(decode_string(intern_table, row->elements[0]) == "1,5");
    auto n = classify_numeric(row->elements[1], heap);
    BOOST_TEST(n.is_fixnum());
    BOOST_TEST(n.int_val == 2);
}

BOOST_AUTO_TEST_CASE(streaming_loop_reclaims_row_objects_after_gc) {
    constexpr std::size_t kRows = 20000;

    Heap heap(1ull << 25);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    std::string input = "a,b\n";
    input.reserve(kRows * 8 + 16);
    for (std::size_t i = 0; i < kRows; ++i) {
        input += "x,1\n";
    }

    const std::size_t before = heap.total_bytes();
    auto reader = call_builtin(env, "%csv-reader-from-string",
        {stringv(heap, intern_table, input), Nil});
    BOOST_REQUIRE(reader.has_value());

    MarkSweepGC gc;
    std::size_t seen = 0;
    while (true) {
        auto row = call_builtin(env, "%csv-read-row", {*reader});
        BOOST_REQUIRE(row.has_value());
        if (*row == False) break;
        ++seen;

        if ((seen % 256) == 0) {
            gc.collect(heap, [&](auto&& visit) {
                visit(*reader);
            });
        }
    }

    BOOST_TEST(seen == kRows);

    auto closed = call_builtin(env, "%csv-close", {*reader});
    BOOST_REQUIRE(closed.has_value());
    BOOST_TEST(*closed == Nil);

    gc.collect(heap, [&](auto&& /*visit*/) {});
    const std::size_t after = heap.total_bytes();
    BOOST_TEST(after <= before + 4096);
}

BOOST_AUTO_TEST_SUITE_END()
