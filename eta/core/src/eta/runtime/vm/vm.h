#pragma once

#include <vector>
#include <stack>
#include <expected>
#include "eta/runtime/nanbox.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/error.h"
#include "bytecode.h"

namespace eta::runtime::vm {

using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::error;

struct Frame {
    const BytecodeFunction* func;
    uint32_t pc;
    uint32_t fp; // index of first local in stack
    LispVal closure;
};

class VM {
public:
    VM(Heap& heap, InternTable& intern_table);

    std::expected<LispVal, RuntimeError> execute(const BytecodeFunction& main);

private:
    Heap& heap_;
    InternTable& intern_table_;
    std::vector<LispVal> stack_;
    std::vector<Frame> frames_;
    std::vector<LispVal> globals_;

    // Current execution state (cached from top frame)
    const BytecodeFunction* current_func_{nullptr};
    uint32_t pc_{0};
    uint32_t fp_{0};
    LispVal current_closure_{0};

    std::expected<void, RuntimeError> run_loop();
    void push(LispVal val) { stack_.push_back(val); }
    LispVal pop() { LispVal v = stack_.back(); stack_.pop_back(); return v; }
};

} // namespace eta::runtime::vm
