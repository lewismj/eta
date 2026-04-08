#pragma once

#include <cstdint>
#include <iomanip>
#include <ostream>
#include <string>

#include "eta/runtime/vm/bytecode.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/semantics/emitter.h"

namespace eta::runtime::vm {

/**
 * @brief Human-readable bytecode disassembler.
 *
 * Prints every BytecodeFunction in objdump-style:
 *   function header (name, arity, stack_size, ...)
 *   numbered instruction stream with decoded operands
 *   constant pool dump
 */
class Disassembler {
public:
    Disassembler(memory::heap::Heap& heap,
                 memory::intern::InternTable& intern_table)
        : heap_(heap), intern_table_(intern_table) {}

    /// Disassemble a single BytecodeFunction.
    void disassemble(const BytecodeFunction& func, std::ostream& os) const {
        // ── Header ──────────────────────────────────────────────
        os << "=== " << (func.name.empty() ? "<anonymous>" : func.name) << " ===\n";
        os << "  arity:      " << func.arity
           << (func.has_rest ? " + rest" : "") << '\n';
        os << "  stack_size: " << func.stack_size << '\n';
        os << "  constants:  " << func.constants.size() << '\n';
        os << "  code:       " << func.code.size() << " instructions\n";

        if (!func.local_names.empty()) {
            os << "  locals:     [";
            for (std::size_t i = 0; i < func.local_names.size(); ++i) {
                if (i) os << ", ";
                os << (func.local_names[i].empty() ? "%" + std::to_string(i) : func.local_names[i]);
            }
            os << "]\n";
        }
        if (!func.upval_names.empty()) {
            os << "  upvals:     [";
            for (std::size_t i = 0; i < func.upval_names.size(); ++i) {
                if (i) os << ", ";
                os << (func.upval_names[i].empty() ? "^" + std::to_string(i) : func.upval_names[i]);
            }
            os << "]\n";
        }

        // ── Constant pool ───────────────────────────────────────
        if (!func.constants.empty()) {
            os << "  -- constant pool --\n";
            for (std::size_t i = 0; i < func.constants.size(); ++i) {
                os << "    [" << i << "] " << format_constant(func.constants[i]) << '\n';
            }
        }

        // ── Instructions ────────────────────────────────────────
        os << "  -- code --\n";
        for (std::size_t i = 0; i < func.code.size(); ++i) {
            const auto& instr = func.code[i];
            os << "    " << std::setw(4) << i << ": "
               << std::left << std::setw(20) << to_string(instr.opcode);

            // Decode meaningful arg for opcodes that use it
            switch (instr.opcode) {
                case OpCode::LoadConst:
                    os << instr.arg;
                    if (instr.arg < func.constants.size())
                        os << "  ; " << format_constant(func.constants[instr.arg]);
                    break;
                case OpCode::MakeClosure: {
                    uint32_t cidx = instr.arg >> 16;
                    uint32_t nup  = instr.arg & 0xFFFF;
                    os << "const=" << cidx << " upvals=" << nup;
                    break;
                }
                case OpCode::SetupCatch: {
                    uint32_t tidx = instr.arg >> 16;
                    uint32_t off  = instr.arg & 0xFFFF;
                    os << "tag=" << tidx << " offset=" << off;
                    break;
                }
                case OpCode::Nop:
                case OpCode::Pop:
                case OpCode::Dup:
                case OpCode::Return:
                case OpCode::Cons:
                case OpCode::Car:
                case OpCode::Cdr:
                case OpCode::Add:
                case OpCode::Sub:
                case OpCode::Mul:
                case OpCode::Div:
                case OpCode::Eq:
                case OpCode::CallWithValues:
                case OpCode::DynamicWind:
                case OpCode::CallCC:
                case OpCode::PopCatch:
                case OpCode::Throw:
                case OpCode::MakeLogicVar:
                case OpCode::Unify:
                case OpCode::DerefLogicVar:
                case OpCode::TrailMark:
                case OpCode::UnwindTrail:
                case OpCode::MakeDual:
                case OpCode::DualVal:
                case OpCode::DualBp:
                    // No arg to display
                    break;
                default:
                    // Generic arg display for Load/Store/Call/Jump etc.
                    os << instr.arg;
                    break;
            }

            // Append source span if available
            if (i < func.source_map.size()) {
                const auto& sp = func.source_map[i];
                if (sp.file_id != 0 || sp.start.line != 0) {
                    os << "  [" << sp.file_id << ":" << sp.start.line << ":" << sp.start.column << "]";
                }
            }

            os << '\n';
        }
        os << '\n';
    }

    /// Disassemble every function in a registry.
    void disassemble_all(const semantics::BytecodeFunctionRegistry& registry,
                         std::ostream& os) const {
        const auto& funcs = registry.all();
        os << "; " << funcs.size() << " function(s)\n\n";
        for (const auto& f : funcs) {
            disassemble(f, os);
        }
    }

private:
    memory::heap::Heap& heap_;
    memory::intern::InternTable& intern_table_;

    /// Pretty-print a NaN-boxed constant for the disassembly listing.
    std::string format_constant(nanbox::LispVal v) const {
        using namespace nanbox;

        if (v == Nil) return "Nil";
        if (v == True) return "#t";
        // False == Nil in this encoding

        if (is_func_index(v))
            return "<func:" + std::to_string(decode_func_index(v)) + ">";

        if (!ops::is_boxed(v)) {
            double d = std::bit_cast<double>(v);
            std::ostringstream oss;
            oss << d;
            return oss.str();
        }

        auto tag = ops::tag(v);
        switch (tag) {
            case Tag::Fixnum: {
                auto val = ops::decode<int64_t>(v);
                return val ? std::to_string(*val) : "?fixnum";
            }
            case Tag::Char: {
                auto val = ops::decode<char32_t>(v);
                if (!val) return "?char";
                if (*val >= 0x20 && *val < 0x7F)
                    return std::string("#\\") + static_cast<char>(*val);
                return "#\\x" + std::to_string(static_cast<uint32_t>(*val));
            }
            case Tag::String: {
                auto sv = intern_table_.get_string(ops::payload(v));
                return sv ? ("\"" + std::string(*sv) + "\"") : "?string";
            }
            case Tag::Symbol: {
                auto sv = intern_table_.get_string(ops::payload(v));
                return sv ? ("'" + std::string(*sv)) : "?symbol";
            }
            case Tag::HeapObject:
                return "<heap:" + std::to_string(ops::payload(v)) + ">";
            case Tag::Nil:
                return (v == True) ? "#t" : "()";
            case Tag::Nan:
                return "NaN";
            default:
                return "?unknown";
        }
    }
};

} // namespace eta::runtime::vm

