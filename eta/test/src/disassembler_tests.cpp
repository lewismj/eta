#include <boost/test/unit_test.hpp>
#include <sstream>

#include "eta/runtime/vm/disassembler.h"
#include "eta/runtime/vm/bytecode.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/semantics/emitter.h"

using namespace eta;
using namespace eta::runtime;
using namespace eta::runtime::vm;
using namespace eta::runtime::nanbox;

struct DisasmFixture {
    memory::heap::Heap heap{1024 * 1024};
    memory::intern::InternTable intern_table;
    Disassembler disasm{heap, intern_table};
};

BOOST_FIXTURE_TEST_SUITE(disassembler_tests, DisasmFixture)

BOOST_AUTO_TEST_CASE(disasm_empty_function) {
    BytecodeFunction func;
    func.name = "empty";
    func.arity = 0;
    func.stack_size = 2;
    func.code.push_back({OpCode::Return, 0});
    func.source_map.push_back({});

    std::ostringstream out;
    disasm.disassemble(func, out);
    std::string text = out.str();
    BOOST_CHECK(text.find("empty") != std::string::npos);
    BOOST_CHECK(text.find("Return") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(disasm_all_opcodes_have_names) {
    /// Verify that to_string covers every opcode we might encounter
    auto check = [](OpCode op) {
        const char* name = to_string(op);
        BOOST_CHECK(name != nullptr);
        BOOST_CHECK(std::string(name) != "Unknown");
    };
    check(OpCode::Nop);
    check(OpCode::LoadConst);
    check(OpCode::LoadLocal);
    check(OpCode::StoreLocal);
    check(OpCode::LoadUpval);
    check(OpCode::StoreUpval);
    check(OpCode::LoadGlobal);
    check(OpCode::StoreGlobal);
    check(OpCode::Pop);
    check(OpCode::Dup);
    check(OpCode::Jump);
    check(OpCode::JumpIfFalse);
    check(OpCode::Call);
    check(OpCode::TailCall);
    check(OpCode::Return);
    check(OpCode::MakeClosure);
    check(OpCode::Cons);
    check(OpCode::Car);
    check(OpCode::Cdr);
    check(OpCode::Add);
    check(OpCode::Sub);
    check(OpCode::Mul);
    check(OpCode::Div);
    check(OpCode::Eq);
    check(OpCode::Values);
    check(OpCode::CallWithValues);
    check(OpCode::DynamicWind);
    check(OpCode::CallCC);
    check(OpCode::PatchClosureUpval);
    check(OpCode::Apply);
    check(OpCode::TailApply);
    check(OpCode::SetupCatch);
    check(OpCode::PopCatch);
    check(OpCode::Throw);
    check(OpCode::MakeLogicVar);
    check(OpCode::Unify);
    check(OpCode::DerefLogicVar);
    check(OpCode::TrailMark);
    check(OpCode::UnwindTrail);
    check(OpCode::CopyTerm);
    check(OpCode::_Reserved1);
    check(OpCode::_Reserved2);
}

BOOST_AUTO_TEST_CASE(disasm_constant_annotation) {
    BytecodeFunction func;
    func.name = "const_test";
    func.arity = 0;
    func.stack_size = 2;
    func.constants.push_back(ops::encode(int64_t{42}).value());
    func.code.push_back({OpCode::LoadConst, 0});
    func.code.push_back({OpCode::Return, 0});
    func.source_map.push_back({});
    func.source_map.push_back({});

    std::ostringstream out;
    disasm.disassemble(func, out);
    std::string text = out.str();
    /// Should contain the constant value annotation
    BOOST_CHECK(text.find("42") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(disasm_function_header) {
    BytecodeFunction func;
    func.name = "my_func";
    func.arity = 3;
    func.has_rest = true;
    func.stack_size = 16;
    func.code.push_back({OpCode::Return, 0});
    func.source_map.push_back({});

    std::ostringstream out;
    disasm.disassemble(func, out);
    std::string text = out.str();
    BOOST_CHECK(text.find("my_func") != std::string::npos);
    BOOST_CHECK(text.find("3") != std::string::npos);
    BOOST_CHECK(text.find("rest") != std::string::npos);
    BOOST_CHECK(text.find("16") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(disasm_instruction_count_matches) {
    BytecodeFunction func;
    func.name = "count";
    func.arity = 0;
    func.stack_size = 4;
    for (int i = 0; i < 5; ++i) {
        func.code.push_back({OpCode::Nop, 0});
        func.source_map.push_back({});
    }
    func.code.push_back({OpCode::Return, 0});
    func.source_map.push_back({});

    std::ostringstream out;
    disasm.disassemble(func, out);
    std::string text = out.str();
    /// Should mention 6 instructions
    BOOST_CHECK(text.find("6 instructions") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(disasm_registry) {
    semantics::BytecodeFunctionRegistry reg;
    for (int i = 0; i < 3; ++i) {
        BytecodeFunction func;
        func.name = "func_" + std::to_string(i);
        func.code.push_back({OpCode::Return, 0});
        func.source_map.push_back({});
        reg.add(std::move(func));
    }

    std::ostringstream out;
    disasm.disassemble_all(reg, out);
    std::string text = out.str();
    BOOST_CHECK(text.find("3 function(s)") != std::string::npos);
    BOOST_CHECK(text.find("func_0") != std::string::npos);
    BOOST_CHECK(text.find("func_1") != std::string::npos);
    BOOST_CHECK(text.find("func_2") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

