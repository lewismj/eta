#include <boost/test/unit_test.hpp>
#include <sstream>

#include "eta/runtime/vm/bytecode_serializer.h"
#include "eta/runtime/vm/bytecode.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/factory.h"
#include "eta/semantics/emitter.h"

// Reuse the emitter fixture for full round-trip tests
#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/reader/expander.h"
#include "eta/reader/module_linker.h"
#include "eta/semantics/semantic_analyzer.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/core_primitives.h"

using namespace eta;
using namespace eta::runtime;
using namespace eta::runtime::vm;
using namespace eta::runtime::nanbox;

// ============================================================================
// Fixture
// ============================================================================

struct SerializerFixture {
    memory::heap::Heap heap{1024 * 1024};
    memory::intern::InternTable intern_table;
    BytecodeSerializer serializer{heap, intern_table};

    // Round-trip helper: serialize then deserialize
    std::expected<EtacFile, SerializerError>
    roundtrip(const std::vector<ModuleEntry>& mods,
              const semantics::BytecodeFunctionRegistry& reg,
              uint64_t hash = 42,
              bool debug = true) {
        std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
        if (!serializer.serialize(mods, reg, hash, debug, ss)) {
            return std::unexpected(SerializerError::IOError);
        }
        ss.seekg(0);
        return serializer.deserialize(ss);
    }
};

BOOST_FIXTURE_TEST_SUITE(bytecode_serializer_tests, SerializerFixture)

// ============================================================================
// Basic round-trips
// ============================================================================

BOOST_AUTO_TEST_CASE(roundtrip_empty_registry) {
    semantics::BytecodeFunctionRegistry reg;
    std::vector<ModuleEntry> mods;
    auto result = roundtrip(mods, reg);
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK_EQUAL(result->modules.size(), 0u);
    BOOST_CHECK_EQUAL(result->registry.size(), 0u);
}

BOOST_AUTO_TEST_CASE(roundtrip_single_function) {
    semantics::BytecodeFunctionRegistry reg;
    BytecodeFunction func;
    func.name = "test_init";
    func.arity = 0;
    func.has_rest = false;
    func.stack_size = 8;
    func.code.push_back({OpCode::LoadConst, 0});
    func.code.push_back({OpCode::Return, 0});
    func.constants.push_back(Nil);
    func.source_map.push_back({});
    func.source_map.push_back({});
    reg.add(std::move(func));

    ModuleEntry mod;
    mod.name = "test";
    mod.init_func_index = 0;
    mod.total_globals = 10;

    auto result = roundtrip({mod}, reg);
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK_EQUAL(result->registry.size(), 1u);
    auto* f = result->registry.get(0);
    BOOST_REQUIRE(f);
    BOOST_CHECK_EQUAL(f->name, "test_init");
    BOOST_CHECK_EQUAL(f->arity, 0u);
    BOOST_CHECK_EQUAL(f->stack_size, 8u);
    BOOST_CHECK_EQUAL(f->code.size(), 2u);
    BOOST_CHECK(f->code[0].opcode == OpCode::LoadConst);
    BOOST_CHECK(f->code[1].opcode == OpCode::Return);
}

BOOST_AUTO_TEST_CASE(roundtrip_all_constant_types) {
    semantics::BytecodeFunctionRegistry reg;
    BytecodeFunction func;
    func.name = "consts";
    func.arity = 0;
    func.stack_size = 4;

    // Nil
    func.constants.push_back(Nil);
    // True
    func.constants.push_back(True);
    // Fixnum
    auto fix = ops::encode(int64_t{12345}).value();
    func.constants.push_back(fix);
    // Double
    auto dbl = ops::encode(3.14).value();
    func.constants.push_back(dbl);
    // Char
    auto ch = ops::encode(char32_t{0x41}).value();
    func.constants.push_back(ch);
    // String
    auto str_id = intern_table.intern("hello").value();
    func.constants.push_back(ops::box(Tag::String, str_id));
    // Symbol
    auto sym_id = intern_table.intern("foo").value();
    func.constants.push_back(ops::box(Tag::Symbol, sym_id));
    // FuncIndex
    func.constants.push_back(encode_func_index(99));

    func.code.push_back({OpCode::Return, 0});
    func.source_map.push_back({});
    reg.add(std::move(func));

    auto result = roundtrip({}, reg);
    BOOST_REQUIRE(result.has_value());
    auto* f = result->registry.get(0);
    BOOST_REQUIRE(f);
    BOOST_CHECK_EQUAL(f->constants.size(), 8u);

    // Nil
    BOOST_CHECK_EQUAL(f->constants[0], Nil);
    // True
    BOOST_CHECK_EQUAL(f->constants[1], True);
    // Fixnum
    auto decoded_fix = ops::decode<int64_t>(f->constants[2]);
    BOOST_CHECK(decoded_fix.has_value());
    BOOST_CHECK_EQUAL(*decoded_fix, 12345);
    // Double
    auto decoded_dbl = ops::decode<double>(f->constants[3]);
    BOOST_CHECK(decoded_dbl.has_value());
    BOOST_CHECK_CLOSE(*decoded_dbl, 3.14, 0.001);
    // Char
    auto decoded_ch = ops::decode<char32_t>(f->constants[4]);
    BOOST_CHECK(decoded_ch.has_value());
    BOOST_CHECK_EQUAL(static_cast<uint32_t>(*decoded_ch), uint32_t{0x41});
    // String
    BOOST_CHECK(ops::is_boxed(f->constants[5]));
    BOOST_CHECK(ops::tag(f->constants[5]) == Tag::String);
    auto sv5 = intern_table.get_string(ops::payload(f->constants[5]));
    BOOST_CHECK(sv5.has_value());
    BOOST_CHECK_EQUAL(std::string(*sv5), "hello");
    // Symbol
    BOOST_CHECK(ops::tag(f->constants[6]) == Tag::Symbol);
    auto sv6 = intern_table.get_string(ops::payload(f->constants[6]));
    BOOST_CHECK(sv6.has_value());
    BOOST_CHECK_EQUAL(std::string(*sv6), "foo");
    // FuncIndex
    BOOST_CHECK(is_func_index(f->constants[7]));
    BOOST_CHECK_EQUAL(decode_func_index(f->constants[7]), 99u);
}

BOOST_AUTO_TEST_CASE(roundtrip_all_opcodes) {
    semantics::BytecodeFunctionRegistry reg;
    BytecodeFunction func;
    func.name = "all_ops";
    func.arity = 0;
    func.stack_size = 4;

    // Add one of each opcode
    func.code.push_back({OpCode::Nop, 0});
    func.code.push_back({OpCode::LoadConst, 0});
    func.code.push_back({OpCode::LoadLocal, 1});
    func.code.push_back({OpCode::StoreLocal, 2});
    func.code.push_back({OpCode::LoadUpval, 3});
    func.code.push_back({OpCode::StoreUpval, 4});
    func.code.push_back({OpCode::LoadGlobal, 5});
    func.code.push_back({OpCode::StoreGlobal, 6});
    func.code.push_back({OpCode::Pop, 0});
    func.code.push_back({OpCode::Dup, 0});
    func.code.push_back({OpCode::Jump, 7});
    func.code.push_back({OpCode::JumpIfFalse, 8});
    func.code.push_back({OpCode::Call, 2});
    func.code.push_back({OpCode::TailCall, 3});
    func.code.push_back({OpCode::Return, 0});
    func.code.push_back({OpCode::MakeClosure, 0});
    func.code.push_back({OpCode::Cons, 0});
    func.code.push_back({OpCode::Car, 0});
    func.code.push_back({OpCode::Cdr, 0});
    func.code.push_back({OpCode::Add, 0});
    func.code.push_back({OpCode::Sub, 0});
    func.code.push_back({OpCode::Mul, 0});
    func.code.push_back({OpCode::Div, 0});
    func.code.push_back({OpCode::Eq, 0});
    func.code.push_back({OpCode::Values, 2});
    func.code.push_back({OpCode::CallWithValues, 0});
    func.code.push_back({OpCode::DynamicWind, 0});
    func.code.push_back({OpCode::CallCC, 0});
    func.code.push_back({OpCode::PatchClosureUpval, 0});
    func.code.push_back({OpCode::Apply, 1});
    func.code.push_back({OpCode::TailApply, 1});
    func.code.push_back({OpCode::SetupCatch, 0});
    func.code.push_back({OpCode::PopCatch, 0});
    func.code.push_back({OpCode::Throw, 0});
    func.code.push_back({OpCode::MakeLogicVar, 0});
    func.code.push_back({OpCode::Unify, 0});
    func.code.push_back({OpCode::DerefLogicVar, 0});
    func.code.push_back({OpCode::TrailMark, 0});
    func.code.push_back({OpCode::UnwindTrail, 0});
    func.code.push_back({OpCode::MakeDual, 0});
    func.code.push_back({OpCode::DualVal, 0});
    func.code.push_back({OpCode::DualBp, 0});

    func.constants.push_back(Nil);
    for (size_t i = 0; i < func.code.size(); ++i)
        func.source_map.push_back({});
    reg.add(std::move(func));

    auto result = roundtrip({}, reg);
    BOOST_REQUIRE(result.has_value());
    auto* f = result->registry.get(0);
    BOOST_REQUIRE(f);
    BOOST_CHECK_EQUAL(f->code.size(), 42u);
    // Spot-check some opcodes
    BOOST_CHECK(f->code[0].opcode == OpCode::Nop);
    BOOST_CHECK(f->code[12].opcode == OpCode::Call);
    BOOST_CHECK_EQUAL(f->code[12].arg, 2u);
    BOOST_CHECK(f->code[14].opcode == OpCode::Return);
    BOOST_CHECK(f->code[41].opcode == OpCode::DualBp);
}

BOOST_AUTO_TEST_CASE(roundtrip_source_map) {
    semantics::BytecodeFunctionRegistry reg;
    BytecodeFunction func;
    func.name = "spans";
    func.arity = 0;
    func.stack_size = 2;
    func.code.push_back({OpCode::Return, 0});
    reader::lexer::Span sp{};
    sp.file_id = 7;
    sp.start.line = 10;
    sp.start.column = 5;
    sp.end.line = 10;
    sp.end.column = 20;
    func.source_map.push_back(sp);
    reg.add(std::move(func));

    auto result = roundtrip({}, reg, 0, /*debug=*/true);
    BOOST_REQUIRE(result.has_value());
    auto* f = result->registry.get(0);
    BOOST_REQUIRE(f);
    BOOST_REQUIRE_EQUAL(f->source_map.size(), 1u);
    BOOST_CHECK_EQUAL(f->source_map[0].file_id, 7u);
    BOOST_CHECK_EQUAL(f->source_map[0].start.line, 10u);
    BOOST_CHECK_EQUAL(f->source_map[0].start.column, 5u);
    BOOST_CHECK_EQUAL(f->source_map[0].end.line, 10u);
    BOOST_CHECK_EQUAL(f->source_map[0].end.column, 20u);
}

BOOST_AUTO_TEST_CASE(roundtrip_local_and_upval_names) {
    semantics::BytecodeFunctionRegistry reg;
    BytecodeFunction func;
    func.name = "names";
    func.arity = 2;
    func.stack_size = 4;
    func.local_names = {"x", "y", "", "temp"};
    func.upval_names = {"captured_a", "captured_b"};
    func.code.push_back({OpCode::Return, 0});
    func.source_map.push_back({});
    reg.add(std::move(func));

    auto result = roundtrip({}, reg);
    BOOST_REQUIRE(result.has_value());
    auto* f = result->registry.get(0);
    BOOST_REQUIRE(f);
    BOOST_REQUIRE_EQUAL(f->local_names.size(), 4u);
    BOOST_CHECK_EQUAL(f->local_names[0], "x");
    BOOST_CHECK_EQUAL(f->local_names[1], "y");
    BOOST_CHECK_EQUAL(f->local_names[2], "");
    BOOST_CHECK_EQUAL(f->local_names[3], "temp");
    BOOST_REQUIRE_EQUAL(f->upval_names.size(), 2u);
    BOOST_CHECK_EQUAL(f->upval_names[0], "captured_a");
    BOOST_CHECK_EQUAL(f->upval_names[1], "captured_b");
}

BOOST_AUTO_TEST_CASE(roundtrip_multiple_functions) {
    semantics::BytecodeFunctionRegistry reg;
    for (int i = 0; i < 5; ++i) {
        BytecodeFunction func;
        func.name = "f" + std::to_string(i);
        func.arity = static_cast<uint32_t>(i);
        func.stack_size = static_cast<uint32_t>(i + 2);
        func.code.push_back({OpCode::Return, 0});
        func.source_map.push_back({});
        reg.add(std::move(func));
    }

    auto result = roundtrip({}, reg);
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK_EQUAL(result->registry.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        auto* f = result->registry.get(static_cast<uint32_t>(i));
        BOOST_REQUIRE(f);
        BOOST_CHECK_EQUAL(f->name, "f" + std::to_string(i));
        BOOST_CHECK_EQUAL(f->arity, static_cast<uint32_t>(i));
    }
}

BOOST_AUTO_TEST_CASE(roundtrip_main_flag) {
    semantics::BytecodeFunctionRegistry reg;
    BytecodeFunction func;
    func.name = "app_init";
    func.code.push_back({OpCode::Return, 0});
    func.source_map.push_back({});
    reg.add(std::move(func));

    ModuleEntry mod;
    mod.name = "app";
    mod.init_func_index = 0;
    mod.total_globals = 20;
    mod.main_func_slot = 15;

    auto result = roundtrip({mod}, reg);
    BOOST_REQUIRE(result.has_value());
    BOOST_REQUIRE_EQUAL(result->modules.size(), 1u);
    BOOST_CHECK_EQUAL(result->modules[0].name, "app");
    BOOST_CHECK(result->modules[0].main_func_slot.has_value());
    BOOST_CHECK_EQUAL(*result->modules[0].main_func_slot, 15u);
}

BOOST_AUTO_TEST_CASE(roundtrip_source_hash) {
    semantics::BytecodeFunctionRegistry reg;
    BytecodeFunction func;
    func.name = "h";
    func.code.push_back({OpCode::Return, 0});
    func.source_map.push_back({});
    reg.add(std::move(func));

    uint64_t expected_hash = 0xDEADBEEFCAFEull;
    auto result = roundtrip({}, reg, expected_hash);
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK_EQUAL(result->source_hash, expected_hash);
}

BOOST_AUTO_TEST_CASE(roundtrip_heap_cons_constant) {
    // Quoted list '(1 2) materializes as heap cons cells
    auto two = ops::encode(int64_t{2}).value();
    auto cons2 = memory::factory::make_cons(heap, two, Nil);
    BOOST_REQUIRE(cons2.has_value());
    auto one = ops::encode(int64_t{1}).value();
    auto cons1 = memory::factory::make_cons(heap, one, *cons2);
    BOOST_REQUIRE(cons1.has_value());

    semantics::BytecodeFunctionRegistry reg;
    BytecodeFunction func;
    func.name = "quoted";
    func.constants.push_back(*cons1);
    func.code.push_back({OpCode::LoadConst, 0});
    func.code.push_back({OpCode::Return, 0});
    func.source_map.push_back({});
    func.source_map.push_back({});
    reg.add(std::move(func));

    auto result = roundtrip({}, reg);
    BOOST_REQUIRE(result.has_value());
    auto* f = result->registry.get(0);
    BOOST_REQUIRE(f);
    BOOST_CHECK_EQUAL(f->constants.size(), 1u);

    // The deserialized constant should be a heap cons
    auto val = f->constants[0];
    BOOST_CHECK(ops::is_boxed(val));
    BOOST_CHECK(ops::tag(val) == Tag::HeapObject);
    auto* c = heap.try_get_as<memory::heap::ObjectKind::Cons, types::Cons>(ops::payload(val));
    BOOST_REQUIRE(c);
    auto car_val = ops::decode<int64_t>(c->car);
    BOOST_CHECK(car_val.has_value());
    BOOST_CHECK_EQUAL(*car_val, 1);
}

// ============================================================================
// Error cases
// ============================================================================

BOOST_AUTO_TEST_CASE(deserialize_bad_magic) {
    std::stringstream ss("JUNK", std::ios::in | std::ios::binary);
    auto result = serializer.deserialize(ss);
    BOOST_CHECK(!result.has_value());
    BOOST_CHECK(result.error() == SerializerError::BadMagic);
}

BOOST_AUTO_TEST_CASE(deserialize_truncated) {
    std::stringstream ss("ETAC", std::ios::in | std::ios::binary);
    auto result = serializer.deserialize(ss);
    BOOST_CHECK(!result.has_value());
    BOOST_CHECK(result.error() == SerializerError::Truncated);
}

BOOST_AUTO_TEST_CASE(deserialize_version_mismatch) {
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    ss.write("ETAC", 4);
    uint16_t bad_ver = 999;
    ss.write(reinterpret_cast<const char*>(&bad_ver), 2);
    // Pad enough to not truncate on version read
    uint16_t flags = 0;
    ss.write(reinterpret_cast<const char*>(&flags), 2);
    ss.seekg(0);
    auto result = serializer.deserialize(ss);
    BOOST_CHECK(!result.has_value());
    BOOST_CHECK(result.error() == SerializerError::VersionMismatch);
}

// ============================================================================
// hash_source
// ============================================================================

BOOST_AUTO_TEST_CASE(hash_source_deterministic) {
    auto h1 = BytecodeSerializer::hash_source("(module hello)");
    auto h2 = BytecodeSerializer::hash_source("(module hello)");
    BOOST_CHECK_EQUAL(h1, h2);
}

BOOST_AUTO_TEST_CASE(hash_source_differs) {
    auto h1 = BytecodeSerializer::hash_source("(module a)");
    auto h2 = BytecodeSerializer::hash_source("(module b)");
    BOOST_CHECK_NE(h1, h2);
}

BOOST_AUTO_TEST_SUITE_END()

